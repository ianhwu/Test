//
//  stitching.hpp
//  SwiftTestPod
//
//  Created by Yan Hu on 2020/9/7.
//  Copyright © 2020 yan. All rights reserved.
//

#ifndef stitching_hpp
#define stitching_hpp

#include <opencv2/opencv.hpp>
#include <stdio.h>

cv::Mat stitch (std::vector<cv::Mat>& images);
cv::Mat stitch2 (std::vector<cv::Mat>& images);

#endif /* stitching_hpp */
