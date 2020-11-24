//
//  CVStitch.hpp
//  SwiftTestPod
//
//  Created by Yan Hu on 2020/9/8.
//  Copyright © 2020 yan. All rights reserved.
//

#ifndef CVStitch_hpp
#define CVStitch_hpp

#include <opencv2/opencv.hpp>
#include <stdio.h>

cv::Mat stitch3(std::vector<cv::Mat>& images);
cv::Mat stitch4(std::vector<cv::Mat>& images);

#endif /* CVStitch_hpp */
