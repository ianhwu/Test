//
//  stitching.cpp
//  SwiftTestPod
//
//  Created by Yan Hu on 2020/9/7.
//  Copyright © 2020 yan. All rights reserved.
//

#include "stitching.hpp"
#include <iostream>

using namespace std;

vector<cv::Mat> imgs;

cv::Mat stitch (vector<cv::Mat>& images)
{
    imgs = images;
    cv::Mat pano;
    cv::Ptr<cv::Stitcher> stitcher = cv::Stitcher::create();
    cv::Stitcher::Status status = stitcher->stitch(imgs, pano);
    
    if (status != cv::Stitcher::OK)
    {
        cout << "Can't stitch images, error code = " << int(status) << endl;
        //return 0;
    }
    return pano;
}

cv::Mat stitch2 (vector<cv::Mat>& images)
{
    imgs = images;
    cv::Mat pano;
    cv::Ptr<cv::Stitcher> stitcher = cv::Stitcher::create();
    int type = 1;
    if(type == 1) { //1:平面拼接
        cv::PlaneWarper* cw = new cv::PlaneWarper();
        stitcher->setWarper(cw);
    } else if(type == 2) { //2:柱面 拼接
        cv::SphericalWarper* cw = new cv::SphericalWarper();
        stitcher->setWarper(cw);
    } else if(type == 3) { //3:立体画面拼接
        cv::StereographicWarper *cw = new cv::StereographicWarper();
        stitcher->setWarper(cw);
    }
    
//    detail::OrbFeaturesFinder *featureFinder = new detail::OrbFeaturesFinder();
//    stitcher->setFeaturesFinder(featureFinder);
//
//    stitcher->setWaveCorrection(false);
    stitcher->setBundleAdjuster(new cv::detail::BundleAdjusterReproj);
//    stitcher->setSeamFinder(new cv::detail::GraphCutSeamFinder());
//    stitcher->setExposureCompensator(new detail::NoExposureCompensator);
//    stitcher->setBlender(new detail::FeatherBlender);
    cv::detail::MultiBandBlender *blender = new cv::detail::MultiBandBlender(false, 5);
    stitcher->setBlender(blender);
    
//    detail::BestOf2NearestMatcher* matcher = new detail::BestOf2NearestMatcher(false, 0.5f);
//    detail::BestOf2NearestRangeMatcher *matcher = new detail::BestOf2NearestRangeMatcher(2, false, 0.5f);
//    stitcher->setFeaturesMatcher(matcher);
    
//    int height = imgs[0].rows / 2;
//    Rect rect1 = Rect(0, imgs[0].rows - height, imgs[0].cols, height);
//    vector<Rect> roi1;
//    roi1.push_back(rect1);
//
//    Rect rect2 = Rect(0, 0, imgs[0].cols, height);
//    vector<Rect> roi2;
//    roi2.push_back(rect2);
//
//    vector<vector<Rect>> rois;
//    rois.resize(2);
//    rois[0] = roi1;
//    rois[1] = roi2;

//    Stitcher::Status status = stitcher->estimateTransform(imgs, rois);
//    Stitcher::Status status = stitcher->estimateTransform(imgs);
    cv::Stitcher::Status status = stitcher->stitch(imgs, pano);
    if (status != cv::Stitcher::OK) {
        cout << "Can't stitch images, error code = " << int(status) << endl;
        return pano;
    }
    
//    status = stitcher->composePanorama(pano);
//    if (status != Stitcher::OK) {
//        cout << "Can't stitch images, error code = " << int(status) << endl;
//    }
    return pano;
}
