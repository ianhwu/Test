//
//  CVWrapper.m
//  CVOpenTemplate
//
//  Created by Washe on 02/01/2013.
//  Copyright (c) 2013 foundry. All rights reserved.
//

#import "stitching.hpp"
#import "CVStitch.hpp"
#import "UIImage+OpenCV.h"
#import "CVCameraView.h"
#import "UIImage+Rotate.h"
#import "CVWrapper.h"

using namespace cv;
@implementation CVWrapper

+ (UIImage *)processImageWithOpenCV: (UIImage *)inputImage {
    NSArray *imageArray = [NSArray arrayWithObject: inputImage];
    UIImage *result = [[self class] processWithArray: imageArray];
    return result;
}

+ (UIImage *)processWithOpenCVImage1:(UIImage *)inputImage1 image2:(UIImage *)inputImage2 {
    NSArray *imageArray = [NSArray arrayWithObjects: inputImage1, inputImage2, nil];
    UIImage *result = [[self class] processWithArray: imageArray];
    return result;
}

+ (UIImage *)processWithArray:(NSArray *)imageArray {
    if ([imageArray count] == 0) {
        NSLog (@"imageArray is empty");
        return 0;
    }
    std::vector<cv::Mat> matImages;

    for (id image in imageArray) {
        if ([image isKindOfClass: [UIImage class]]) {
            /*
             All images taken with the iPhone/iPa cameras are LANDSCAPE LEFT orientation. The  UIImage imageOrientation flag is an instruction to the OS to transform the image during display only. When we feed images into openCV, they need to be the actual orientation that we expect them to be for stitching. So we rotate the actual pixel matrix here if required.
             */
            UIImage* rotatedImage = [image rotateToImageOrientation];
            cv::Mat matImage = [rotatedImage CVMat3];
            NSLog (@"matImage: %@", image);
            matImages.push_back(matImage);
        }
    }
    NSLog (@"stitching...");
    cv::Mat stitchedMat = stitch3(matImages);
    UIImage* result =  [UIImage imageWithCVMat: stitchedMat];
    return result;
}

+ (void)run {
    NSString *path = [[NSBundle mainBundle] pathForResource:@"a" ofType:@"jpg"];
    std::string file = std::string([path UTF8String]);
    Mat A = imread(file);
    print(A);
}

+ (UIImageView *)cameraView {
    CVCameraView *view = [[CVCameraView alloc] init];
    return view;
}

@end


