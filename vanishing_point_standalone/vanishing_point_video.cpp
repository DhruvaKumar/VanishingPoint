/**
 * @file vanishing_point_video.cpp
 * @brief Detecting vanishing point in a video stream
 * @author Dhruva Kumar
 */

#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include "opencv2/opencv.hpp"
#include <iostream>
#include <stdio.h>  
#include <string>
#include <time.h>

 #define BILLION 1000000000L

// #include "FlyCapture2.h"

 using namespace cv;
 using namespace std;

/* ---------------------------------- Parameters ----------------------------------*/
 Mat frame, edges;
 Mat frame_gray;
 Mat standard_hough;
 int min_threshold = 50;
 int max_trackbar = 150;
  // image dimensions
 int width = 640;
 int height = 480;

 // canny 
 int lowThreshold = 60;
 int const max_lowThreshold = 100;
 int ratio = 3;
 int kernel_size = 3;
 // hough
 int s_trackbar = 30;
 const char* standard_name = "Standard Hough Lines Demo";
 // ransac parameters
 int N_iterations = 50; // # of iterations for ransac
 int threshold_ransac = 10; // distance within which the hypothesis is classified as an inlier

 // moving average parameters
 const int movingAvg_window = 10;
 int window [movingAvg_window][2] = {};
 int lp_pointer = 0;
 int running_sum [2] = {};

 // lpf parameters
 Point vp_prev, vp_filter_prev;
 bool initFlag_vp, initFlag_mid = true;
 int freq_sampling = 10;
 int freq_c = 20;
 
 unsigned long ind = 0;
 vector<int> compression_params;

  uint64_t diff;
  struct timespec start, end;

 // Function Headers
 void help();
 void Standard_Hough( int, void* );

/* -------------------------------------- main --------------------------------------------*/
 int main( int argc, char** argv )
 {
  // read the video
  string filename = "input.avi";
  VideoCapture capture(filename);

  if( !capture.isOpened() )
    throw "Error when reading video";

  compression_params.push_back(CV_IMWRITE_PNG_COMPRESSION);
  compression_params.push_back(0);

    // capture loop
    char key = 0;
    while(key != 'q')
    {
        capture >> frame;
        if(frame.empty())
            break;

        // convert the frame to grayscale
        cvtColor( frame, frame_gray, COLOR_RGB2GRAY );

        // create trackbars(canny & hough) for thresholds
        char thresh_label[50];
        sprintf( thresh_label, "Hough thresh: %d + input", min_threshold );

        namedWindow( standard_name, WINDOW_AUTOSIZE );
        //createTrackbar( "Canny threshold", standard_name, &lowThreshold, max_lowThreshold, Standard_Hough);
        //createTrackbar( thresh_label, standard_name, &s_trackbar, max_trackbar, Standard_Hough);
        
        // initialize
        Standard_Hough(0, 0);

        key = cv::waitKey(30);        
    }

    return 0;
}

/* -------------------------------------- findIntersectingPt --------------------------------------------*/
// find intersecting point between two lines using crammer's rule. 
// if no intersecting point (parallel lines/same line) return false
// i/p: lines: [rho_1;theta_1] & [rho_2;theta_2] and intersectingPt
bool findIntersectingPoint(float r_1, float t_1, float r_2, float t_2, Point& intersectingPt) 
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

int findInliers(const vector<Vec2f>& s_lines, const Point& intersectingPt) 
{
  int inliers = 0;
  //print//cout << "Distance: ";
  for (int i = 0; i < static_cast<int>(s_lines.size()); i++) {
    //Mat tmp = tempImg.clone();
    // find error: shortest distance between intersectingPt and line
    float r = s_lines[i][0], t = s_lines[i][1];
    double a = cos(t), b = sin(t);
    int x = intersectingPt.x, y = intersectingPt.y;
    double d = abs(a*x + b*y - r) / sqrt(pow(a,2) + pow(b,2));

    // // debugging
    // double alpha = 1000;
    // double cos_t = cos(t), sin_t = sin(t);
    // double x0 = r*cos_t, y0 = r*sin_t;
    // Point pt1( cvRound(x0 + alpha*(-sin_t)), cvRound(y0 + alpha*cos_t) );
    // Point pt2( cvRound(x0 - alpha*(-sin_t)), cvRound(y0 - alpha*cos_t) );
    // line( tmp, pt1, pt2, Scalar(0,255,255), 2, CV_AA);

    // imshow("temp_", tmp);
    // waitKey(0);

    // find inliers
    if (d < threshold_ransac) { inliers++; }
  }
  //print//cout << endl;
  return inliers;
}

/* -----------------------------------------Moving average-----------------------------------------*/

 // // moving average parameters
 // const int movingAvg_window = 20;
 // int window [movingAvg_window][2] = {};
 // int lp_pointer = 0;
 // int sum [2] = {};
// input vp gets filtered and saved in vp_filter
void movingAvg (Point& vp_filter, const Point& vp) 
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
  // int *avg = (int *) malloc(sizeof (int) * 2);
  // avg[0] = (int) (float)running_sum[0]/movingAvg_window;
  // avg[1] = (int) (float)running_sum[1]/movingAvg_window;
  vp_filter.x = (int) (float)running_sum[0]/movingAvg_window;
  vp_filter.y = (int) (float)running_sum[1]/movingAvg_window;

  // increment lp_pointer and keep within bounds
  lp_pointer++;
  if (lp_pointer >= movingAvg_window) lp_pointer = 0;

}



/* ---------------------1st order LPF (discretized using tustin approx)--------------------------*/
// input vp gets filtered and saved in vp_filter
// int freq_sampling | int freq_c
void lpf (Point& vp_filter, const Point& vp, bool initFlag) 
{
  // first time initialization
  if (initFlag) 
  {
    vp_filter_prev.x = vp.x;
    vp_prev.x = vp.x;
    vp_filter.x = vp.x;
    vp_filter_prev.y = vp.y;
    vp_prev.y = vp.y;
    vp_filter.y = vp.y;
    initFlag = false;
    return;
  }

  float Tw = 1.0/freq_sampling * 2.0 * CV_PI * freq_c;
  vp_filter.x = (int) (Tw*(vp.x + vp_prev.x) - (Tw-2)*vp_filter_prev.x)/(Tw+2);
  vp_filter.y = (int) (Tw*(vp.y + vp_prev.y) - (Tw-2)*vp_filter_prev.y)/(Tw+2);

  return;
}

/* ------------------------------------- middle point ------------------------------------------*/
// Takes in the 2 best lines and computes the middle point between the intersection of those lines with the x-axis
int computeMiddlePt(int a_best, int b_best, const vector<Vec2f>& s_lines) 
{
  // corner case: take care of a_best / b_best out of bounds
  if (a_best < 0 || a_best > static_cast<int>(s_lines.size()) || b_best < 0 || b_best > static_cast<int>(s_lines.size())) 
  {
    return (int) (width/2.0);
  }

  Point intersectingPt;
  float r_1, t_1, r_2, t_2;
  int x1, x2;
  bool found;
  r_1 = s_lines[a_best][0]; t_1 = s_lines[a_best][1]; // 1st line
  r_2 = height/2.0; t_2 = CV_PI/2; // horizontal line
  
  found = findIntersectingPoint(r_1, t_1, r_2, t_2, intersectingPt);
  // if intersecting point not found set it at the left border of the image
  if (found) 
  { 
    x1 = intersectingPt.x;
    // limit
    if (x1<0) x1 = 0;
    if (x1>width) x1 = width; 
  }
  else 
  {
    if (t_1 * 180/CV_PI < CV_PI/2 ) { x1 = 0; } 
    else { x1 = width; }
  }
  
  r_1 = s_lines[b_best][0]; t_1 = s_lines[b_best][1]; // 2nd line
  found= findIntersectingPoint(r_1, t_1, r_2, t_2, intersectingPt);
  // if intersecting point not found set it at the right border of the image
  if (found) 
  { 
    x2 = intersectingPt.x; 
    // limit
    if (x2<0) x2 = 0;
    if (x2>width) x2 = width; 
  }
  else 
  {
    if (t_1 * 180/CV_PI < CV_PI/2 ) { x2 = 0; } 
    else { x2 = width; }
  }

  int x_m = (int) (x1 + x2)/2.0;
// int x_m = 0;
  // limit within bounds
  if (x_m > width) x_m = width;
  if (x_m < 0) x_m = 0;

  return x_m;

}

/* -------------------------------------- vp detection --------------------------------------------*/
 void Standard_Hough( int, void* )
 {
  vector<Vec2f> s_lines;

  
  // 1(a) Reduce noise with a kernel 3x3
  blur( frame, edges, Size(3,3) );

  // clock_gettime(CLOCK_MONOTONIC, &start); /* mark start time */
  // 1(b) Apply Canny edge detector
  Canny( edges, edges, lowThreshold, lowThreshold*ratio, kernel_size);

  // clock_gettime(CLOCK_MONOTONIC, &end); /* mark the end time */
  // diff = BILLION * (end.tv_sec - start.tv_sec) + end.tv_nsec - start.tv_nsec;
  // printf(" edge = %llu ns", (long long unsigned int) diff);

  cvtColor( edges, standard_hough, CV_GRAY2BGR );

  // clock_gettime(CLOCK_MONOTONIC, &start); /* mark start time */
  // 2. Use Standard Hough Transform
  HoughLines(edges, s_lines, 1, CV_PI/180, min_threshold + s_trackbar, 0, 0 );
  // clock_gettime(CLOCK_MONOTONIC, &end); /* mark the end time */
  // diff = BILLION * (end.tv_sec - start.tv_sec) + end.tv_nsec - start.tv_nsec;
  // printf(" hough = %llu ns", (long long unsigned int) diff);

  //preprocessing: remove vertical lines within +-10 degrees
  for (int i = static_cast<int> (s_lines.size()) - 1; i>=0; i--) 
  {
    int t_deg = (int) s_lines[i][1] * 180.0/CV_PI;
    // it looks like theta ranges from 0 to 180 degrees
    if (t_deg < 10 || t_deg > 170)  s_lines.erase(s_lines.begin() + i);
    
  }
  

  ////////////////////////////////////////////////////
  /// Show the result
  for( size_t i = 0; i < s_lines.size(); i++ )
  {
    float r = s_lines[i][0], t = s_lines[i][1];
    double cos_t = cos(t), sin_t = sin(t);
    double x0 = r*cos_t, y0 = r*sin_t;
    double alpha = 1000;

    Point pt1( cvRound(x0 + alpha*(-sin_t)), cvRound(y0 + alpha*cos_t) );
    Point pt2( cvRound(x0 - alpha*(-sin_t)), cvRound(y0 - alpha*cos_t) );
    line( standard_hough, pt1, pt2, Scalar(255,0,0), 0.5, CV_AA);
  }
  ////////////////////////////////////////////////////

  // split the lines into 2 lists based on theta. ransac will randomly (not so random) choose 2 lines
  // from the 2 lists respectively. 
  vector<int> lines_1, lines_2;
  for (vector<int>::size_type i = 0; i!=s_lines.size(); i++)
  {
    int t_deg = (int) s_lines[i][1] * 180.0/CV_PI;
    if (t_deg < CV_PI/2.0) lines_1.push_back(i);
    else lines_2.push_back(i);
  }

  // clock_gettime(CLOCK_MONOTONIC, &start); /* mark start time */
  // 3. RANSAC if > 2 lines available
  if (static_cast<int>(s_lines.size()) > 1) 
  {
    // ransac
    // input: s_lines | 
    Mat tempImg;
    //cout << "Size of image: " << standard_hough.rows << ", "<< standard_hough.cols << endl;
    int maxInliers = 0; int a_best, b_best;
    Point vp, vp_filter, vp_filter2;
    for (int i = 0; i<N_iterations; i++) 
    {
      tempImg = standard_hough.clone();;
      // 1. randomly select 2 lines
      // edit: not so random. chose lines from 2 buckets categorized according to theta
      // if the list is not empty
      int a,b;
      if (!lines_1.empty() && !lines_2.empty())
      {
        a = rand() % static_cast<int>(lines_1.size());
        a = lines_1[a];
        b = rand() % static_cast<int>(lines_2.size());
        b = lines_2[b];
      } else {
        a = rand() % static_cast<int>(s_lines.size());
        b = rand() % static_cast<int>(s_lines.size());
      }

      // 2. find intersecting point (x_v, y_v)
      Point intersectingPt;
      float r_1 = s_lines[a][0], t_1 = s_lines[a][1];
      float r_2 = s_lines[b][0], t_2 = s_lines[b][1];
      bool found= findIntersectingPoint(r_1, t_1, r_2, t_2, intersectingPt);

      // skip if not found
      if (!found) continue;
      

    // // // draw pairs of lines and intersecting pt for debugging purposes
//     double alpha = 1000;
//     double cos_t = cos(t_1), sin_t = sin(t_1);
//     Point pt1( cvRound(r_1*cos_t + alpha*(-sin_t)), cvRound(r_1*sin_t + alpha*cos_t) );
//     Point pt2( cvRound(r_1*cos_t - alpha*(-sin_t)), cvRound(r_1*sin_t - alpha*cos_t) );
//     line(tempImg, pt1, pt2, Scalar(255,255,0), 2, CV_AA);
// // 
//     cos_t = cos(t_2); sin_t = sin(t_2);
//     Point pt11( cvRound(r_2*cos_t + alpha*(-sin_t)), cvRound(r_2*sin_t + alpha*cos_t) );
//     Point pt22( cvRound(r_2*cos_t - alpha*(-sin_t)), cvRound(r_2*sin_t - alpha*cos_t) );
//     line(tempImg, pt11, pt22, Scalar(255,255,0), 2, CV_AA);

//     int x_m = computeMiddlePt(a, b, s_lines);
//     Point mid;
//     mid.x = x_m;
//     mid.y = (int) height/2.0;

//     // horizontal line
//     Point pt1_h( 0, cvRound(height/2.0));
//     Point pt2_h( width, cvRound(height/2.0));
//     line( tempImg, pt1_h, pt2_h, Scalar(0,255,255), 1, CV_AA);

//     circle(tempImg, intersectingPt, 2,  Scalar(0,0,255), 2, 8, 0 );
//     circle(tempImg, mid, 2,  Scalar(0,255,255), 2, 8, 0 );
//     imshow("temp", tempImg);
//     waitKey(0);

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
        a_best = a;
        b_best = b;
      }

    } // end of ransac iterations
    
    // limit vanishing point to be within image bounds
    if (vp.x > width) vp.x = width;
    if (vp.x < 0) vp.x = 0;
    if (vp.y > height) vp.y = height;
    if (vp.y < 0) vp.y = 0;

    int x_m;  Point mid;
    ////////////////////////////////////////////////////////////////////////
//     Point intersectingPt;
//       float r_1 = s_lines[a_best][0], t_1 = s_lines[a_best][1];
//       float r_2 = s_lines[b_best][0], t_2 = s_lines[b_best][1];
//       bool found= findIntersectingPoint(r_1, t_1, r_2, t_2, intersectingPt);

//     // // // draw pairs of lines and intersecting pt for debugging purposes
//     double alpha = 1000;
//     double cos_t = cos(t_1), sin_t = sin(t_1);
//     Point pt1( cvRound(r_1*cos_t + alpha*(-sin_t)), cvRound(r_1*sin_t + alpha*cos_t) );
//     Point pt2( cvRound(r_1*cos_t - alpha*(-sin_t)), cvRound(r_1*sin_t - alpha*cos_t) );
//     line(tempImg, pt1, pt2, Scalar(0,0,255), 2, CV_AA);
// // 
//     cos_t = cos(t_2); sin_t = sin(t_2);
//     Point pt11( cvRound(r_2*cos_t + alpha*(-sin_t)), cvRound(r_2*sin_t + alpha*cos_t) );
//     Point pt22( cvRound(r_2*cos_t - alpha*(-sin_t)), cvRound(r_2*sin_t - alpha*cos_t) );
//     line(tempImg, pt11, pt22, Scalar(0,0,255), 2, CV_AA);

//     x_m = computeMiddlePt(a_best, b_best, s_lines);
   
//     mid.x = x_m;
//     mid.y = (int) height/2.0;

//     // horizontal line
//     Point pt1_h( 0, cvRound(height/2.0));
//     Point pt2_h( width, cvRound(height/2.0));
//     line( tempImg, pt1_h, pt2_h, Scalar(0,255,255), 1, CV_AA);

//     circle(tempImg, intersectingPt, 2,  Scalar(0,0,255), 2, 8, 0 );
//     circle(tempImg, mid, 2,  Scalar(0,255,255), 2, 8, 0 );
//     imshow("temp", tempImg);
//     waitKey(0);
    //////////////////////////////////////////////////////////////////////////

    // compute middle point x_m
    x_m = computeMiddlePt(a_best, b_best, s_lines);
    Point mid_filter; 
    mid.x = x_m;
    mid.y = (int) height/2.0;

    // apply moving average/lpf filter over frames for vp
    // movingAvg(vp_filter, vp);
    // movingAvg(mid_filter, mid);
    lpf(vp_filter, vp, initFlag_vp);
    lpf(mid_filter, mid, initFlag_mid);

    // compute error signal 
    int error = (vp_filter.x - cvRound(width/2.0));
    // int error2 = (vp_filter2.x - cvRound(width/2.0));
    cout << "Vanishing point = " << vp.x << "," << vp.y << "| Inliers: " << maxInliers << "| error: "<< error << endl;

    // plot vanishing point on images
    // lines
    float r_1 = s_lines[a_best][0], t_1 = s_lines[a_best][1];
    float r_2 = s_lines[b_best][0], t_2 = s_lines[b_best][1];
    double alpha = 1000;
    double cos_t = cos(t_1), sin_t = sin(t_1);
    Point pt1( cvRound(r_1*cos_t + alpha*(-sin_t)), cvRound(r_1*sin_t + alpha*cos_t) );
    Point pt2( cvRound(r_1*cos_t - alpha*(-sin_t)), cvRound(r_1*sin_t - alpha*cos_t) );
    line(standard_hough, pt1, pt2, Scalar(0,0,255), 1, CV_AA);
// 
    cos_t = cos(t_2); sin_t = sin(t_2);
    Point pt11( cvRound(r_2*cos_t + alpha*(-sin_t)), cvRound(r_2*sin_t + alpha*cos_t) );
    Point pt22( cvRound(r_2*cos_t - alpha*(-sin_t)), cvRound(r_2*sin_t - alpha*cos_t) );
    line(standard_hough, pt11, pt22, Scalar(0,0,255), 1, CV_AA);

    // circle(standard_hough, vp, 2,  Scalar(0,0,255), 2, 8, 0 );
    // circle(frame, vp, 3,  Scalar(0,0,255), 2, 8, 0 );
    // circle(standard_hough, vp_filter, 2,  Scalar(0,255,0), 2, 8, 0 );
    // circle(standard_hough, mid_filter, 2,  Scalar(0,0,255), 2, 8, 0 );
    circle(standard_hough, vp, 3,  Scalar(0,255,0), 2, 8, 0 );
    circle(standard_hough, mid, 3,  Scalar(0,0,255), 2, 8, 0 );
    circle(frame, vp, 3,  Scalar(0,255,0), 2, 8, 0 );
    circle(frame, mid, 3,  Scalar(0,0,255), 2, 8, 0 );
    // circle(frame, vp_filter, 3,  Scalar(0,0,255), 2, 8, 0 );
    // circle(frame, mid_filter, 3,  Scalar(0,0,255), 2, 8, 0 );

  } // end of ransac (if > 2 lines available)
  // clock_gettime(CLOCK_MONOTONIC, &end); /* mark the end time */
  // diff = BILLION * (end.tv_sec - start.tv_sec) + end.tv_nsec - start.tv_nsec;
  // printf(" ransac = %llu ns", (long long unsigned int) diff);

  // draw cross hair
  Point pt1_v( cvRound(width/2.0), 0);
  Point pt2_v( cvRound(width/2.0), height);
  line( standard_hough, pt1_v, pt2_v, Scalar(0,255,255), 1, CV_AA);
  Point pt1_h( 0, cvRound(height/2.0));
  Point pt2_h( width, cvRound(height/2.0));
  line( standard_hough, pt1_h, pt2_h, Scalar(0,255,255), 1, CV_AA);

  imshow( standard_name, standard_hough );
  imshow("Original", frame);
  // waitKey(0);

  // write image to disk
  // char str[50], str2[50];
  // int vp_error = (vp.x - cvRound(width/2.0));
  // int mid_error = (mid.x - cvRound(width/2.0));
  // sprintf(str, "images_og/image_%lu_%d_%d.png",ind, vp_error, mid_error);
  // sprintf(str2, "images_behind_the_scenes/image_%lu_%d_%d.png",ind, vp_error, mid_error);
  // imwrite( str, frame , compression_params);
  // imwrite( str2, standard_hough, compression_params );
  // cout << "Writing image " << ind << endl;
  // ind++;


}


