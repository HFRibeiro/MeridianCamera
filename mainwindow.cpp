#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QDomDocument>
#include <opencv2/opencv.hpp>
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"

#define MAX_NETIF					8
#define MAX_CAMERAS_PER_NETIF	32
#define MAX_CAMERAS		(MAX_NETIF * MAX_CAMERAS_PER_NETIF)

// Enable/disable Bayer to RGB conversion
// (If disabled - Bayer format will be treated as Monochrome).
#define ENABLE_BAYER_CONVERSION 1

// Enable/disable buffer FULL/EMPTY handling (cycling)
#define USE_SYNCHRONOUS_BUFFER_CYCLING	0

// Enable/disable transfer tuning (buffering, timeouts, thread affinity).
#define TUNE_STREAMING_THREADS 0

#define NUM_BUF	8
void *m_latestBuffer = NULL;

using namespace cv;
using namespace std;

using namespace cv;
using namespace std;

list<long> timeStamps;
long startTime = time(0);
list<Point> positionStamps;
list<float> stopes;

int predictionLineStart = 0;
int predictionLineEnd = 0;

// Values for thresholding
int iLowH = 0;
int iHighH = 179;
int iLowS = 0;
int iHighS = 255;
int iLowV = 100;
int iHighV = 255;

int capDelay = 30;

int iMiddleX;	// X value for the center of the frame

bool bTracking = false;		// Is a Star being tracked?
bool trackingStart = false;

int iMouseX;	// Mouse Click Position X axis
int iMouseY;	// Mouse Click Position Y axis

// Global Image Matrixes
Mat imgOriginal;
Mat imgToProcess;
Mat imgStaticOverlay;
Mat imgVarOverlay;
Mat imgVarOverlayTmp;

bool bDoneProcessing = true;	// Boolean flag for thread processing
bool bStarCrossed = false;		// Flipped when the star crosses the meridian

int meridianPoint;		// Point in the meridian where the star crossed

// More Globals
std::string starName, starHour, starMin, starSec, starHSec, starDirection, fileName;
int cameraId, comId, comBaudRate;


//
// Gets the center and radius of a circle from 3 points
// http://stackoverflow.com/questions/20698613/detect-semi-circle-in-opencv
//
inline void getCircle(cv::Point& p1, cv::Point& p2, cv::Point& p3, cv::Point& center, float& radius)
{
    float x1 = p1.x;
    float x2 = p2.x;
    float x3 = p3.x;

    float y1 = p1.y;
    float y2 = p2.y;
    float y3 = p3.y;

    // PLEASE CHECK FOR TYPOS IN THE FORMULA :)
    center.x = (x1*x1 + y1*y1)*(y2 - y3) + (x2*x2 + y2*y2)*(y3 - y1) + (x3*x3 + y3*y3)*(y1 - y2);
    center.x /= (2 * (x1*(y2 - y3) - y1*(x2 - x3) + x2*y3 - x3*y2));

    center.y = (x1*x1 + y1*y1)*(x3 - x2) + (x2*x2 + y2*y2)*(x1 - x3) + (x3*x3 + y3*y3)*(x2 - x1);
    center.y /= (2 * (x1*(y2 - y3) - y1*(x2 - x3) + x2*y3 - x3*y2));

    radius = sqrt((center.x - x1)*(center.x - x1) + (center.y - y1)*(center.y - y1));
}

// This function is called whenever there is a mouse click on the window
void callBack(int event, int x, int y, int flags, void* userdata)
{
    if (event == EVENT_LBUTTONDOWN)
    {
        cout << "Left button of the mouse is clicked - position (" << x << ", " << y << ")" << endl;
        iMouseX = x;
        iMouseY = y;
        bTracking = true;

        startTime = time(0);
        timeStamps.clear();

        positionStamps.clear();
        stopes.clear();

        predictionLineStart = 0;
        predictionLineEnd = 0;

        trackingStart = true;
    }
}

// Main image processing function
void imageProcessing() {

    long now = time(0) - startTime;
    timeStamps.push_back(now);

    Mat imgHSV;
    cvtColor(imgToProcess, imgHSV, COLOR_BGR2HSV); // convert the captured frame from BGR to HSV

    Mat imgThresholded;
    inRange(imgHSV, Scalar(iLowH, iLowS, iLowV), Scalar(iHighH, iHighS, iHighV), imgThresholded); // threshold the image

    // morphological opening (removes small objects from the foreground)
    erode(imgThresholded, imgThresholded, getStructuringElement(MORPH_ELLIPSE, Size(5, 5)));
    dilate(imgThresholded, imgThresholded, getStructuringElement(MORPH_ELLIPSE, Size(5, 5)));

    // morphological closing (removes small holes from the foreground)
    dilate(imgThresholded, imgThresholded, getStructuringElement(MORPH_ELLIPSE, Size(5, 5)));
    erode(imgThresholded, imgThresholded, getStructuringElement(MORPH_ELLIPSE, Size(5, 5)));

    Mat vLabels;
    Mat vStats;
    Mat centroids;	// Detected lights' center points

    // find every connected component in the image (detected lights)
    int iComponents = connectedComponentsWithStats(imgThresholded, vLabels, vStats, centroids, 8, CV_32S);

    //cout << iComponents << endl;
    int iSelectedStarX = -1;
    int iSelectedStarY = -1;

    double dShortestDistance = 99999;
    for (int i = 1; i < iComponents; i++)
    {
        int iStarX = centroids.at<double>(i, 0);
        int iStarY = centroids.at<double>(i, 1);

        circle(imgVarOverlayTmp, Point(iStarX, iStarY), 20, Scalar(0, 0, 50), 1, LINE_AA, 0);

        double res = norm(Point(iStarX, iStarY) - Point(iMouseX, iMouseY));
        if (res < dShortestDistance) {
            dShortestDistance = res;
            iSelectedStarX = iStarX;
            iSelectedStarY = iStarY;
        }
    }

    double res = norm(Point(iMouseX, iMouseY) - Point(iSelectedStarX, iSelectedStarY));

    Point previousPosition = Point(iMouseX, iMouseY);
    if (res < 25) {
        iMouseX = iSelectedStarX;
        iMouseY = iSelectedStarY;
    }

    circle(imgVarOverlayTmp, Point(iMouseX, iMouseY), 20, Scalar(0, 0, 150), 2, LINE_AA, 0);

    if (trackingStart) {
        trackingStart = false;
        bDoneProcessing = true;
        return;
    }

    if (positionStamps.size() > 1 && iMouseX != previousPosition.x) {

        float stope = (previousPosition.y - iMouseY) / (previousPosition.x - iMouseX);
        stopes.push_back(stope);
        list<float>::iterator it;
        float averageStope = 0;
        for (it = stopes.begin(); it != stopes.end(); ++it) {
            //cout << "stope: " << *it << endl;
            averageStope = averageStope + *it;
        }
        averageStope = averageStope / stopes.size();

        //cout << "m: " << averageStope << endl;

        Point p1;
        Point p2;
        Point p3;

        list<Point>::iterator itt;
        Point averagePosition;
        int pos = 0;
        for (itt = positionStamps.begin(); itt != positionStamps.end(); ++itt) {
            Point p = *itt;
            averagePosition.x = averagePosition.x + p.x;
            averagePosition.y = averagePosition.y + p.y;

            if (pos == 0) p1 = p;
            if (pos == positionStamps.size() / 2) p2 = p;
            if (pos == positionStamps.size() - 1) p3 = p;
            pos++;
        }
        averagePosition.x = averagePosition.x / positionStamps.size();
        averagePosition.y = averagePosition.y / positionStamps.size();

        float b = averagePosition.y - averageStope*averagePosition.x;
        //float b = iMouseY - averageStope*iMouseX;

        // cout << "b: " << b << endl;
        // y = mx + b
        // b = y - mx

        predictionLineStart = averageStope * 0 + b;
        predictionLineEnd = averageStope * iMiddleX * 2 + b;

        Point center;
        float radius;

        getCircle(p1, averagePosition, p3, center, radius);

    }


    positionStamps.push_back(Point(iMouseX, iMouseY));

    imshow("Thresholded Image", imgThresholded); //show the thresholded image


    if (
        ( starDirection == "LR" && previousPosition.x < iMiddleX && iSelectedStarX >= iMiddleX)
        ||
        ( starDirection == "RL" && previousPosition.x > iMiddleX && iSelectedStarX <= iMiddleX) ) {
            circle(imgStaticOverlay, Point(iMiddleX, iSelectedStarY), 5, Scalar(0, 75, 0), -1, LINE_AA, 0);
            cout << " *****" << endl;
            meridianPoint = iSelectedStarY;
            bStarCrossed = true;
    }


    bDoneProcessing = true;
}

std::string getTimeNow()
{
    typedef std::chrono::system_clock Clock;

    auto now = Clock::now();
    auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);

    auto fraction = now - seconds;
    time_t cnow = Clock::to_time_t(now);

    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(fraction);

    const std::time_t now_aux = std::time(nullptr); // get the current time point
    const std::tm calendar_time = *std::localtime(std::addressof(now_aux));

    char buffer[80];
    sprintf(buffer, "%d\n%d\n%d\n%d\n", calendar_time.tm_hour, calendar_time.tm_min, calendar_time.tm_sec,(int)milliseconds.count() / 10);

    std::string timeNow = "";

    timeNow.append(buffer);

    return timeNow;

}


MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    QString filename = "/home/observatorio/QtProjects/MeridianCamera/config.xml";
    QDomDocument doc;
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly) || !doc.setContent(&file));

    QDomNodeList xml_tags = doc.elementsByTagName("star");
    for (int i = 0; i < xml_tags.size(); i++) {
        QDomNode n = xml_tags.item(i);
        QDomElement name = n.firstChildElement("name");
        QDomElement direction = n.firstChildElement("direction");
        QDomElement hours = n.firstChildElement("hours");
        QDomElement minutes = n.firstChildElement("minutes");
        QDomElement seconds = n.firstChildElement("seconds");

        if(!name.isNull()) starName = name.text().toStdString();
        if(!direction.isNull()) starDirection = direction.text().toStdString();
        if(!hours.isNull()) starHour = hours.text().toStdString();
        if(!minutes.isNull()) starMin = minutes.text().toStdString();
        if(!seconds.isNull()) starSec = seconds.text().toStdString();
    }

    qDebug() << "starName" << QString::fromStdString(starName);
    qDebug() << "starDirection" << QString::fromStdString(starDirection);
    qDebug() << "starHour" << QString::fromStdString(starHour);
    qDebug() << "starMin" << QString::fromStdString(starMin);
    qDebug() << "starSec" << QString::fromStdString(starSec);

    xml_tags = doc.elementsByTagName("camera");
    for (int i = 0; i < xml_tags.size(); i++) {
        QDomNode n = xml_tags.item(i);
        QDomElement id = n.firstChildElement("id");
        QDomElement minV = n.firstChildElement("minV");
        QDomElement maxV = n.firstChildElement("maxV");

        if(!id.isNull()) cameraId = id.text().toInt();
        if(!minV.isNull()) iLowV = minV.text().toInt();
        if(!maxV.isNull()) iHighV = maxV.text().toInt();
    }

    qDebug() << "cameraId" << QString::number(cameraId);
    qDebug() << "iLowV" << QString::number(iLowV);
    qDebug() << "iHighV" << QString::number(iHighV);

    xml_tags = doc.elementsByTagName("file");
    for (int i = 0; i < xml_tags.size(); i++) {
        QDomNode n = xml_tags.item(i);
        QDomElement name = n.firstChildElement("name");
        QDomElement location = n.firstChildElement("location");
        if(!name.isNull() && !location.isNull())
        {
            QString fileNameAux = location.text()+name.text();
            fileName = fileNameAux.toStdString();
        }
    }

    qDebug() << "fileName" << QString::fromStdString(fileName);

    starHSec = "00";

    std::cout << "Using camera device " << cameraId << std::endl;
    std::cout << "Tracking " << starName << std::endl;
    std::cout << "Using Serial Port: COM" << comId << std::endl;


    GEV_CAMERA_HANDLE camHandle;
    GEV_BUFFER_OBJECT *img = NULL;
    GEV_STATUS status = 0;

    GEVLIB_CONFIG_OPTIONS options = {0};

    GEV_DEVICE_INTERFACE  pCamera[MAX_CAMERAS] = {0};
    int numCamera = 0;
    int camIndex = 0;
   pthread_t  tid;
    char c;
    int done = FALSE;
    int turboDriveAvailable = 0;
    char uniqueName[128];
    uint32_t macLow = 0; // Low 32-bits of the mac address (for file naming).

    GevGetLibraryConfigOptions( &options);
    //options.logLevel = GEV_LOG_LEVEL_OFF;
    //options.logLevel = GEV_LOG_LEVEL_TRACE;
    options.logLevel = GEV_LOG_LEVEL_NORMAL;
    GevSetLibraryConfigOptions( &options);

    // Wait for images to be received
    //status = GevWaitForNextImage(camHandle, &img, 1000);

    status = GevGetCameraList( pCamera, MAX_CAMERAS, &numCamera);

    qDebug() << numCamera << "camera(s) on the network";
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::on_bt_start_clicked()
{
    runing = true;

    VideoCapture cap;	// Capture device (camera or file)
    cap.open(fileName);

    // double rate = cap.get(CV_CAP_PROP_FPS);

    // Create a window called "Control" to house trackbars and switches
    namedWindow("Control", CV_WINDOW_AUTOSIZE);
    createTrackbar("LowV", "Control", &iLowV, 255);		// Value (0 - 255)
    createTrackbar("HighV", "Control", &iHighV, 255);
    createTrackbar("Capture Delay (ms)", "Control", &capDelay, 5000);

    // TODO find out why this isn't implemented
    // createButton("button1", lightCircleButton, NULL, QT_CHECKBOX, 0);

    Mat imgTmp;
    cap.read(imgTmp);	// Capture a temporary image from the camera

    bool bSuccess = cap.read(imgTmp); // Read a new frame from capture device
    if (!bSuccess) // If unsuccessful, exit application
    {
        cerr << "Cannot read a test frame from video stream" << endl;
        cerr << "exiting..." << endl;
    }

    // Create a black images with the size of the capture device for drawing
    imgStaticOverlay = Mat::zeros(imgTmp.size(), CV_8UC3);
    imgVarOverlay = Mat::zeros(imgTmp.size(), CV_8UC3);
    imgVarOverlayTmp = Mat::zeros(imgTmp.size(), CV_8UC3);

    // Calculate the pixel value of the center of the X axis of the camera
    iMiddleX = imgTmp.size().width / 2;
    cout << "middle x " << iMiddleX << endl;

    // Boolean flags for UI elements
    bool bStaticOverlay = true;
    bool bVarOverlay = true;

    // Create windows
    namedWindow("Main Window", CV_WINDOW_AUTOSIZE);
    namedWindow("Thresholded Image", CV_WINDOW_AUTOSIZE);

    //cvSetWindowProperty("Main Window", CV_WND_PROP_FULLSCREEN, CV_WINDOW_FULLSCREEN);


    line(imgStaticOverlay, Point(iMiddleX, 0), Point(iMiddleX, imgTmp.size().height), Scalar(0, 75, 0), 1, LINE_8);

    // Main loop
    while (runing) {

        namedWindow("Main Window", CV_WINDOW_KEEPRATIO);
        namedWindow("Thresholded Image", CV_WINDOW_KEEPRATIO);
        // Set callback function for any mouse event on the Main Window
        setMouseCallback("Main Window", callBack, NULL);

        Mat imgOriginal;
        bool bSuccess = cap.read(imgOriginal); // read a new frame from video
        if (!bSuccess) // if not successful, break loop
        {
            cerr << "Cannot read a frame from video stream" << endl;
            cerr << "exiting..." << endl;
            break;
        }

        Mat imgComposed;
        imgOriginal.copyTo(imgComposed);

        if (bDoneProcessing) {

            if (bStarCrossed) {
                bStarCrossed = false;
                qDebug() << "Play sound now!";
                QSound::play("/home/observatorio/QtProjects/MeridianCamera/sound.wav");
            }

            imgOriginal.copyTo(imgToProcess);
            imgVarOverlay = imgVarOverlayTmp.clone();
            imgVarOverlayTmp = Mat::zeros(imgTmp.size(), CV_8UC3);

            bDoneProcessing = false;
            imageProcessing();
        }

        if (bStaticOverlay) {
            imgComposed = imgComposed + imgStaticOverlay;
        }

        if (bVarOverlay) {
            imgComposed = imgComposed + imgVarOverlay;
        }

        imshow("Main Window", imgComposed); //show the composed image

        int key = waitKey(capDelay + 1);
        if (key  == 27) //wait for 'esc' key press for 30ms. If 'esc' key is pressed, break loop
        {
            cout << "ESC key is pressed by user" << endl;
            cout << "exiting..." << endl;
            break;
            // TODO WAIT FOR PENDING THREADS
        }
        else if (key == 8) {
            cout << "teste" << endl;
            cap.release();
            cap.open(fileName);
        }
    }

    //destroy GUI windows
    destroyAllWindows();
}

void MainWindow::on_bt_stop_clicked()
{
    runing = false;
    destroyAllWindows();
}
