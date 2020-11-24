//
//  CVStitch.cpp
//  SwiftTestPod
//
//  Created by Yan Hu on 2020/9/8.
//  Copyright © 2020 yan. All rights reserved.
//

#include "CVStitch.hpp"
#include <iostream>

using namespace cv;
using namespace std;

void OptimizeSeam(Mat& img1, Mat& trans, Mat& dst);

typedef std::set<std::pair<int,int> > MatchesSet;
 
//计算原始图像点位在经过矩阵变换后在目标图像上对应位置
Point2f getTransformPoint(const Point2f originalPoint,const Mat &transformMaxtri);
vector<cv::Mat> stitch3Imgs;
Mat stitch3(vector<cv::Mat>& images) {
    stitch3Imgs = images;
    Mat image01 = stitch3Imgs[0];
    Mat image02 = stitch3Imgs[1];
 
    //灰度图转换
    Mat image1,image2;
    cvtColor(image01, image1, COLOR_RGB2GRAY);
    cvtColor(image02, image2, COLOR_RGB2GRAY);
    
    //特征点描述，为下边的特征点匹配做准备
    vector<KeyPoint> keyPoint1, keyPoint2;
    Mat imageDesc1, imageDesc2;
    
    
    cv::Ptr<cv::ORB> ptrBrisk = cv::ORB::create(500);
    
    ptrBrisk->detectAndCompute( image1, noArray(), keyPoint1, imageDesc1 );
    ptrBrisk->detectAndCompute( image2, noArray(), keyPoint2, imageDesc2 );
    
    
    Ptr<DescriptorMatcher> matcher = makePtr<FlannBasedMatcher>(makePtr<flann::LshIndexParams>(12, 20, 2));

    
    vector<vector<DMatch> > matchePoints;
    vector<DMatch> GoodMatchePoints;
    
//    MatchesSet matches;
    matcher->knnMatch( imageDesc1, imageDesc2, matchePoints, 1);
    
    if (matchePoints.size() == 0) {
        return image01;
    }
    
    // Lowe's algorithm,获取优秀匹配点
    for (int i = 0; i < matchePoints.size(); i++) {
        if (matchePoints[i].size() > 0) {
            if (matchePoints[i][0].distance < 0.75 * matchePoints[i][1].distance) {
                GoodMatchePoints.push_back(matchePoints[i][0]);
            }
        }
    }
    cout<< "\n1->2 matches: " << GoodMatchePoints.size() << endl;
    
    if (GoodMatchePoints.size() < 4) {
        return image01;
    }
    
    //将两张图像转换为同一坐标下 变换矩阵
    vector<Point2f> imagePoints1, imagePoints2;
    float maxY = 0;
    for (int i = 0; i < GoodMatchePoints.size(); i++) {
        imagePoints1.push_back(keyPoint1[GoodMatchePoints[i].queryIdx].pt);
        imagePoints2.push_back(keyPoint2[GoodMatchePoints[i].trainIdx].pt);
        
        float y = keyPoint1[GoodMatchePoints[i].queryIdx].pt.y;
        if (y > maxY) {
            maxY = y;
        }
    }
    
    //获取图像1到图像2的投影映射矩阵 尺寸为 3 * 3
    cout << "findHomography: " << imagePoints1.size() << endl;
    Mat homo = findHomography(imagePoints1, imagePoints2, RANSAC);
//    cout << "变换矩阵为：\n" << homo << endl << endl; //输出映射矩阵
    
    //计算配准图的四个顶点坐标
    //-- Get the corners from the image_1 ( the object to be "detected" )
    std::vector<Point2f> obj_corners(4);
    obj_corners[0] = Point2f(0, 0);
    obj_corners[1] = Point2f( (float)image01.cols, 0 );
    obj_corners[2] = Point2f( (float)image01.cols, (float)image01.rows );
    obj_corners[3] = Point2f( 0, (float)image01.rows );
    std::vector<Point2f> scene_corners(4);
    perspectiveTransform(obj_corners, scene_corners, homo);
    
    
    //画 匹配特征点的 连线
//    Mat img_matches;
//    drawMatches( image01, keyPoint1, image02, keyPoint2, GoodMatchePoints, img_matches, Scalar::all(-1),
//    Scalar::all(-1), std::vector<char>(), DrawMatchesFlags::NOT_DRAW_SINGLE_POINTS );
//
//    line( img_matches, scene_corners[0],
//          scene_corners[1], Scalar(0, 255, 0), 4 ); // 绿
//
//    line( img_matches, scene_corners[1],
//          scene_corners[2], Scalar( 255, 0, 0), 4 ); // 红
//
//    line( img_matches, scene_corners[2],
//          scene_corners[3], Scalar( 0, 0, 255), 4 ); // 蓝
//
//    line( img_matches, scene_corners[3],
//          scene_corners[0], Scalar( 0, 0, 0), 4 ); // 黑
    
    cout << scene_corners << endl;
    
//    return img_matches;
    
    float y = scene_corners[2].y;
    //  图片1 起始位置
    float valueY = image01.rows - abs(y);

    
    // 图片1 截取掉的长度
    float image01_cut = image01.rows - valueY;
    
    // 图片2 截取之后剩余的长度
    float image02_left = image02.rows - image01_cut;
    
    // 重叠的长度
    float repeat = image01_cut - (image01.rows - maxY);
    
    cout << "image01_cut:" << image01_cut << "   image02_left:" << image02_left << "   repeat:" << repeat << endl;
    
    if (repeat <= 0) {
        return image01;
    }
    
    //创建拼接后的图,需提前计算图的大小
    float dst_width = image01.cols;  // 取最右点的长度为拼接图的长度
    float dst_height = image01.rows + image02_left;
    
    cout << "width:" << dst_width << "   height:" << dst_height << endl;
    if (image02_left <= 0) {
        return image01;
    }
    
    //图像配准
    Mat imageTransform1;
    warpPerspective(image01, imageTransform1, homo, Size(image01.cols, repeat));
    
    
    Mat dst(dst_height, dst_width, CV_8UC3);
    dst.setTo(0);
    
//    if (y < 0) {
        image01.copyTo(dst(cv::Rect(0, 0, image01.cols, image01.rows)));
        image02.copyTo(dst(cv::Rect(0, valueY, image02.cols, image02.rows)));
        cout << "copyToT1:" << imageTransform1.cols << "   " << imageTransform1.rows << endl;
        imageTransform1.copyTo(dst(cv::Rect(0, valueY, imageTransform1.cols, imageTransform1.rows)));
//    } else {
//        image02.copyTo(dst(cv::Rect(0, valueY, image02.cols, image02.rows)));
//        image01.copyTo(dst(cv::Rect(0, 0, image01.cols, image01.rows)));
//        cout << "copyToT1:" << imageTransform1.cols << "   " << imageTransform1.rows << endl;
//        imageTransform1.copyTo(dst(cv::Rect(0, valueY, imageTransform1.cols, imageTransform1.rows)));
//    }
    
    return dst;
}
 
//计算原始图像点位在经过矩阵变换后在目标图像上对应位置
Point2f getTransformPoint(const Point2f originalPoint,const Mat &transformMaxtri) {
    Mat originelP,targetP;
    originelP=(Mat_<double>(3,1)<<originalPoint.x,originalPoint.y,1.0);
    targetP=transformMaxtri*originelP;
    float x=targetP.at<double>(0,0)/targetP.at<double>(2,0);
    float y=targetP.at<double>(1,0)/targetP.at<double>(2,0);
    return Point2f(x,y);
}

Mat stitch4(vector<cv::Mat>& images) {
//    Mat img_object = imread( samples::findFile( parser.get<String>("input1") ), IMREAD_GRAYSCALE );
//    Mat img_scene = imread( samples::findFile( parser.get<String>("input2") ), IMREAD_GRAYSCALE );
    
    stitch3Imgs = images;
    Mat image01 = stitch3Imgs[0];
    Mat image02 = stitch3Imgs[1];
    
    //灰度图转换
    Mat img_object,img_scene;
    cvtColor(image01, img_object, IMREAD_GRAYSCALE);
    cvtColor(image02, img_scene, IMREAD_GRAYSCALE);
    
    //-- Step 1: Detect the keypoints using SURF Detector, compute the descriptors
//    int minHessian = 400;
//    Ptr<SURF> detector = SURF::create( minHessian );
    
    cv::Ptr<cv::ORB> ptrBrisk = cv::ORB::create(500);
    
    std::vector<KeyPoint> keypoints_object, keypoints_scene;
    Mat descriptors_object, descriptors_scene;
    
    ptrBrisk->detectAndCompute( img_object, noArray(), keypoints_object, descriptors_object );
    ptrBrisk->detectAndCompute( img_scene, noArray(), keypoints_scene, descriptors_scene );
    
    //-- Step 2: Matching descriptor vectors with a FLANN based matcher
    // Since SURF is a floating-point descriptor NORM_L2 is used
//    Ptr<DescriptorMatcher> matcher = DescriptorMatcher::create(DescriptorMatcher::FLANNBASED);
    Ptr<DescriptorMatcher> matcher = makePtr<FlannBasedMatcher>(makePtr<flann::LshIndexParams>(12, 20, 2));
    std::vector< std::vector<DMatch> > knn_matches;
    matcher->knnMatch( descriptors_object, descriptors_scene, knn_matches, 1 );
    //-- Filter matches using the Lowe's ratio test
    const float ratio_thresh = 0.1f;
    std::vector<DMatch> good_matches;
    
    for (size_t i = 0; i < knn_matches.size(); i++)
    {
        float offset = knn_matches[i][0].queryIdx * 1.0 / knn_matches[i][0].trainIdx;
        cout << "offset: " << offset << endl;
        if (knn_matches[i][0].distance < ratio_thresh * knn_matches[i][1].distance &&
            (offset > 1.8))
        {
            good_matches.push_back(knn_matches[i][0]);
        }
    }
    //-- Draw matches
    Mat img_matches;
    drawMatches( img_object, keypoints_object, img_scene, keypoints_scene, good_matches, img_matches, Scalar::all(-1),
                 Scalar::all(-1), std::vector<char>(), DrawMatchesFlags::NOT_DRAW_SINGLE_POINTS );
    //-- Localize the object
    std::vector<Point2f> obj;
    std::vector<Point2f> scene;
    
    int maxY = 0, minY = 0;
    for( size_t i = 0; i < good_matches.size(); i++ )
    {
        //-- Get the keypoints from the good matches
        obj.push_back( keypoints_object[ good_matches[i].queryIdx ].pt );
        scene.push_back( keypoints_scene[ good_matches[i].trainIdx ].pt );
        
        int y = keypoints_object[ good_matches[i].queryIdx ].pt.y;
        
        if (y > maxY) {
            maxY = y;
        } else if (y < minY || minY == 0) {
            minY = y;
        }
    }
    Mat H = findHomography( obj, scene, RANSAC );
    //-- Get the corners from the image_1 ( the object to be "detected" )
    std::vector<Point2f> obj_corners(4);
    obj_corners[0] = Point2f(0, 0);
    obj_corners[1] = Point2f( (float)img_object.cols, 0 );
    obj_corners[2] = Point2f( (float)img_object.cols, (float)img_object.rows );
    obj_corners[3] = Point2f( 0, (float)img_object.rows );
    std::vector<Point2f> scene_corners(4);
    perspectiveTransform( obj_corners, scene_corners, H);
    //-- Draw lines between the corners (the mapped object in the scene - image_2 )
    line( img_matches, scene_corners[0] + Point2f((float)img_object.cols, 0),
          scene_corners[1] + Point2f((float)img_object.cols, 0), Scalar(0, 255, 0), 4 );
    line( img_matches, scene_corners[1] + Point2f((float)img_object.cols, 0),
          scene_corners[2] + Point2f((float)img_object.cols, 0), Scalar( 0, 255, 0), 4 );
    line( img_matches, scene_corners[2] + Point2f((float)img_object.cols, 0),
          scene_corners[3] + Point2f((float)img_object.cols, 0), Scalar( 0, 255, 0), 4 );
    line( img_matches, scene_corners[3] + Point2f((float)img_object.cols, 0),
          scene_corners[0] + Point2f((float)img_object.cols, 0), Scalar( 0, 255, 0), 4 );
    cout << scene_corners << endl;
    return img_matches;
}

