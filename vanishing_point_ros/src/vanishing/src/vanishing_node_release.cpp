#include <ros/ros.h>
#include <image_transport/image_transport.h>
#include <cv_bridge/cv_bridge.h>
#include <sensor_msgs/image_encodings.h>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>
#include "std_msgs/Float32.h"
#include <iostream>
#include <stdio.h>  
#include <string>

#define LP_WINDOW 10
// debugging flag to display images
#define DISPLAY_IMG 0

static const std::string OPENCV_WINDOW = "Vanishing point";
// topic where the error is being published
static const std::string VP_TOPIC = "vanishing_point_topic";
// topic where the image is being published
static const std::string VP_IMG_TOPIC = "/vp/output_video";


using namespace cv;
using namespace std;

class VanishingPoint
{
  ros::NodeHandle nh_;
  image_transport::ImageTransport it_;
  image_transport::Subscriber image_sub_;
  image_transport::Publisher image_pub_;
  ros::Publisher vp_pub_; 
  std_msgs::Int16 error_;
  cv_bridge::CvImagePtr cv_ptr_;
  cv_bridge::CvImage out_msg_;

  // vanishing point algo parameters
  Mat frame, edges;
  Mat standard_hough;
  int min_threshold;
  int max_trackbar;
  // canny 
  int lowThreshold;
  int ratio;
  int kernel_size;
  // hough
  int s_trackbar;
  // image dimensions
  int width;
  int height;
  // ransac parameters
  int N_iterations; // # of iterations for ransac
  int threshold_ransac; // distance within which the hypothesis is classified as an inlier
  // moving average parameters
  int **window;
  int lp_pointer;
  int *running_sum;
  // lpf parameters
  Point vp_prev, vp_lp_prev;
  bool initFlag; 
  int freq_sampling; 
  int freq_c;

public:

  VanishingPoint(): it_(nh_)
  {
    // create a publisher object with topic: vanishing point
    vp_pub_ = nh_.advertise<std_msgs::Float32>(VP_TOPIC, 1000);
    // Subscribe to input video feed and publish output video feed
    image_sub_ = it_.subscribe("/camera/image_raw", 1, &VanishingPoint::imageCB, this);
    image_pub_ = it_.advertise(VP_IMG_TOPIC, 1);

    // init vp parameters
    min_threshold = 50;
    max_trackbar = 150;
    // canny 
    lowThreshold = 60;
    ratio = 3;
    kernel_size = 3;
    // hough
    s_trackbar = 50;
    // image dimensions
    width = 640;
    height = 480;
    // ransac parameters
    N_iterations = 100; // # of iterations for ransac
    threshold_ransac = 10; // distance within which the hypothesis is classified as an inlier
    // moving avg parameters
    lp_pointer = 0;
    window = new int*[LP_WINDOW];
    for (int i = 0; i < LP_WINDOW; i++) { 
      window[i] = new int[2](); 
    }
    running_sum = new int[2]();
    // lpf params
    initFlag; = true;
    freq_sampling; = 25;
    freq_c; = 40;
  } 

  ~VanishingPoint()
  {
    if (DISPLAY_IMG) { cv::destroyWindow(OPENCV_WINDOW); }
    // delete arrays to avoid memory leaks
    for (int i = 0; i<2; i++) {
      delete [] window[i];
    }
    delete [] window;
    delete [] running_sum;
  }

  // callback
  void imageCB(const sensor_msgs::ImageConstPtr& msg)
  {
    // convert ROS raw image to CV::mat (mono8) mono16 required?
    
    try
    {
      cv_ptr_ = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::MONO8);
    }
    catch (cv_bridge::Exception& e)
    {
      ROS_ERROR("cv_bridge exception: %s", e.what());
      return;
    }

    frame = cv_ptr_->image;
    vp_detection();

    cv::waitKey(30);

    // // compute vanishing point and error
    // error_.data = 10;
    // ROS_INFO("%d", error_.data);

    // // publish the error
    // vp_pub_.publish(error_);

  }

private:
  void vp_detection();
  bool findIntersectingPoint(float, float, float, float, Point&);
  int findInliers(const vector<Vec2f>&, const Point&);
  void movingAvg (Point&, const Point&);
  void lpf (Point&, const Point&);
};


/* -------------------------------------- vp detection --------------------------------------------*/
void VanishingPoint::vp_detection() 
{
  vector<Vec2f> s_lines;

  // 1(a) Reduce noise with a kernel 3x3
  blur( frame, edges, Size(3,3) );

  // 1(b) Apply Canny edge detector
  Canny( edges, edges, lowThreshold, lowThreshold*ratio, kernel_size);
  cvtColor( edges, standard_hough, CV_GRAY2BGR );

  // 2. Use Standard Hough Transform
  HoughLines(edges, s_lines, 1, CV_PI/180, min_threshold + s_trackbar, 0, 0 );

  //preprocessing: remove vertical lines within +-5 degrees
  for (int i = static_cast<int> (s_lines.size()) - 1; i>=0; i--) 
  {
    int t_deg = s_lines[i][1] * 180/CV_PI;
    // it looks like theta ranges from 0 to 180 degrees
    if (t_deg < 5 || t_deg > 175)  s_lines.erase(s_lines.begin() + i);
  }

  // 3. RANSAC if > 2 lines available
  if (static_cast<int>(s_lines.size()) > 1) 
  {
    // ransac
    //Mat tempImg;
    int maxInliers = 0;
    Point vp, vp_lp;
    for (int i = 0; i<N_iterations; i++) 
    {
      //tempImg = standard_hough.clone();;
      // 1. randomly select 2 lines
      int a = rand() % static_cast<int>(s_lines.size());
      int b = rand() % static_cast<int>(s_lines.size());

      // 2. find intersecting point (x_v, y_v)
      Point intersectingPt;
      float r_1 = s_lines[a][0], t_1 = s_lines[a][1];
      float r_2 = s_lines[b][0], t_2 = s_lines[b][1];
      bool found= findIntersectingPoint(r_1, t_1, r_2, t_2, intersectingPt);

      // skip if not found
      if (!found) continue;

      // 3. find error for each line (shortest distance b/w point above and line: perpendicular bisector)
      // 4. find # inliers (error < threshold)
      //int inliers = findInliers(s_lines, intersectingPt, tempImg);
      int inliers = findInliers(s_lines, intersectingPt);
      //print//cout << "Num of inliers: " << inliers << endl;

      // 5. if # inliers > maxInliers, save model
      if (inliers > maxInliers) 
      {
        maxInliers = inliers;
        vp.x = intersectingPt.x; 
        vp.y = intersectingPt.y;
      }

    } // end of ransac iterations
    
    // limit vanishing point to be within image bounds
    if (vp.x > width) vp.x = width;
    if (vp.x < 0) vp.x = 0;
    if (vp.y > height) vp.y = height;
    if (vp.y < 0) vp.y = 0;

    // apply moving average/lpf filter over frames
    // movingAvg(vp_lp, vp);
    lpf(vp_lp, vp);

    // compute error signal 
    float error = (vp_lp.x - cvRound(width/2.0)) / width;
    // cout << "Vanishing point = " << vp.x << "," << vp.y << "| Inliers: " << maxInliers << "| error: "<< error << endl;
    // cout << "VP_LP_x: " << vp_lp.x << " | " << "center: " << cvRound(width/2.0) << endl;
    error_.data = error;
    ROS_INFO("Error: %.4f | VP_LP_x: %d | center_x: %d ", error_.data, vp_lp.x, (width/2.0));

    // publish the error to topic defined before (vanishing_point_topic)
    vp_pub_.publish(error_);

    // plot vanishing point on images
    if (DISPLAY_IMG)
    {
      circle(standard_hough, vp, 2,  Scalar(0,0,255), 2, 8, 0 );
      circle(frame, vp, 3,  Scalar(0,0,255), 2, 8, 0 );
      circle(standard_hough, vp_lp, 2,  Scalar(0,255,0), 2, 8, 0 );
      circle(frame, vp_lp, 3,  Scalar(0,255,0), 2, 8, 0 );
    }

  } // end of ransac (if > 2 lines available)

  // display edge+hough+vp for degbugging
  // draw cross hair
  if (DISPLAY_IMG)
  {
    Point pt1_v( cvRound(width/2.0), 0);
    Point pt2_v( cvRound(width/2.0), height);
    line( standard_hough, pt1_v, pt2_v, Scalar(0,255,255), 1, CV_AA);
    Point pt1_h( 0, cvRound(height/2.0));
    Point pt2_h( width, cvRound(height/2.0));
    line( standard_hough, pt1_h, pt2_h, Scalar(0,255,255), 1, CV_AA);

    imshow( "houghlines", standard_hough );
    imshow("Original", frame);
  }
  
  // Debugging: Output modified video stream
  //out_msg_->header = cv_ptr_->header;
  //out_msg_->encoding = sensor_msgs::image_encodings::BGR8;
  //out_msg_->image = standard_hough;

  //out_msg_.header = cv_ptr_->header;
  //out_msg_.encoding = sensor_msgs::image_encodings::BGR8;
  //out_msg_.image = standard_hough;

  //image_pub_.publish(out_msg_.toImageMsg());
}

/* -------------------------------------- findIntersectingPt --------------------------------------------*/
// find intersecting point between two lines using crammer's rule. 
// if no intersecting point (parallel lines/same line) return false
// i/p: lines: [rho_1;theta_1] & [rho_2;theta_2] and intersectingPt
bool VanishingPoint::findIntersectingPoint(float r_1, float t_1, float r_2, float t_2, Point& intersectingPt) 
{
  double determinant = (cos(t_1) * sin(t_2)) - (cos(t_2) * sin(t_1));
  if (determinant != 0) {
    intersectingPt.x = (int) (sin(t_2)*r_1 - sin(t_1)*r_2) / determinant;
    intersectingPt.y = (int) (cos(t_1)*r_2 - cos(t_2)*r_1) / determinant;
    return true;
  }
  // else no point found (parallel lines/same line) 
  return false;
}

/* ------------------------------------------findInliers --------------------------------------------*/

int VanishingPoint::findInliers(const vector<Vec2f>& s_lines, const Point& intersectingPt) 
{
  int inliers = 0;
  for (int i = 0; i < static_cast<int>(s_lines.size()); i++) {
    //Mat tmp = tempImg.clone();
    // find error: shortest distance between intersectingPt and line
    float r = s_lines[i][0], t = s_lines[i][1];
    double a = cos(t), b = sin(t);
    int x = intersectingPt.x, y = intersectingPt.y;
    double d = abs(a*x + b*y - r) / sqrt(pow(a,2) + pow(b,2));

    // find inliers
    if (d < threshold_ransac) { inliers++; }
  }

  return inliers;
}

/* -----------------------------------------Moving average-----------------------------------------*/

 // // low pass parameters
 // const int LP_WINDOW = 20;
 // int window [LP_WINDOW][2] = {};
 // int lp_pointer = 0;
 // int sum [2] = {};
 // input vp gets filtered and saved in vp_lp
void VanishingPoint::movingAvg (Point& vp_lp, const Point& vp) 
{
  // remove element from running sum
  running_sum[0] -= window[lp_pointer][0];
  running_sum[1] -= window[lp_pointer][1];

  // update window with new vanishing point
  window[lp_pointer][0] = vp.x;
  window[lp_pointer][1] = vp.y;

  // update sum
  running_sum[0] += window[lp_pointer][0];
  running_sum[1] += window[lp_pointer][1];

  // update running avg
  vp_lp.x = (int) (float)running_sum[0]/movingAvg_window;
  vp_lp.y = (int) (float)running_sum[1]/movingAvg_window;

  // increment lp_pointer and keep within bounds
  lp_pointer++;
  if (lp_pointer >= movingAvg_window) lp_pointer = 0;
}

/* ---------------------1st order LPF (discretized using tustin approx)--------------------------*/
// input vp gets filtered and saved in vp_lp
// int freq_sampling | int freq_c
void VanishingPoint::lpf (Point& vp_lp, const Point& vp) 
{
  // first time initialization
  if (initFlag) 
  {
    vp_lp_prev.x = vp.x;
    vp_prev.x = vp.x;
    vp_lp.x = vp.x;
    vp_lp_prev.y = vp.y;
    vp_prev.y = vp.y;
    vp_lp.y = vp.y;
    initFlag = false;
    return;
  }

  float Tw = 1.0/freq_sampling * 2.0 * CV_PI * freq_c;
  vp_lp.x = (int) (Tw*(vp.x + vp_prev.x) - (Tw-2)*vp_lp_prev.x)/(Tw+2);
  vp_lp.y = (int) (Tw*(vp.y + vp_prev.y) - (Tw-2)*vp_lp_prev.y)/(Tw+2);

  return;
}


/* -------------------------------------- main --------------------------------------------*/

int main(int argc, char** argv)
{
  // initialize ros node for publishing vanishing point error
  ros::init(argc, argv, "vanishing_point_publisher");

  VanishingPoint vp;
  ros::spin();
  return 0;

}
