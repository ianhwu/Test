//
//  CVCameraView.m
//  SwiftTestPod
//
//  Created by Yan Hu on 2020/9/15.
//  Copyright © 2020 yan. All rights reserved.
//

//#import <opencv2/videoio/cap_ios.h>
//#import <opencv2/opencv.hpp>
#import "CVCameraView.h"

//using namespace cv;
//
//@interface CVCameraView() <CvVideoCameraDelegate>
//
//@property (nonatomic, strong) CvVideoCamera* videoCamera;
//
//@end

@implementation CVCameraView

//- (instancetype)initWithFrame:(CGRect)frame
//{
//    self = [super initWithFrame:frame];
//    if (self) {
//        self.videoCamera = [[CvVideoCamera alloc] initWithParentView: self];
//        self.videoCamera.defaultAVCaptureDevicePosition = AVCaptureDevicePositionFront;
//        self.videoCamera.defaultAVCaptureSessionPreset = AVCaptureSessionPresetHigh;
//        self.videoCamera.defaultAVCaptureVideoOrientation = AVCaptureVideoOrientationPortrait;
//        self.videoCamera.defaultFPS = 30;
//        self.videoCamera.grayscaleMode = NO;
//        self.videoCamera.delegate = self;
//        [self.videoCamera start];
//    }
//    return self;
//}
//
//- (void)processImage:(cv::Mat &)image {
//    // Do some OpenCV stuff with the image
////    Mat image_copy;
////    cvtColor(image, image_copy, COLOR_BGR2GRAY);
//    // invert image
////    bitwise_not(image_copy, image_copy);
////    Convert BGR to BGRA (three channel to four channel)
////    Mat bgr;
////    cvtColor(image, bgr, COLOR_GRAY2BGR);
////    cvtColor(bgr, image, COLOR_BGR2BGRA);
//    Mat rgba;
//    cvtColor(image, rgba, COLOR_RGBA2BGRA);
//    id image1 = [self imageWithCVMat:rgba];
//    dispatch_async(dispatch_get_main_queue(), ^{
//        self.image = image1;
//    });
//}
//
//- (id)imageWithCVMat:(const cv::Mat&)cvMat {
//    NSData *data = [NSData dataWithBytes:cvMat.data length:cvMat.elemSize() * cvMat.total()];
//    CGColorSpaceRef colorSpace;
//
//    if (cvMat.elemSize() == 1) {
//        colorSpace = CGColorSpaceCreateDeviceGray();
//    } else {
//        colorSpace = CGColorSpaceCreateDeviceRGB();
//    }
//
//    CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
//
//        // Creating CGImage from cv::Mat
//    CGImageRef imageRef = CGImageCreate(cvMat.cols,                                 //width
//                                        cvMat.rows,                                 //height
//                                        8,                                          //bits per component
//                                        8 * cvMat.elemSize(),                       //bits per pixel
//                                        cvMat.step[0],                              //bytesPerRow
//                                        colorSpace,                                 //colorspace
//                                        kCGImageAlphaNoneSkipLast|
//                                        kCGBitmapByteOrderDefault,// bitmap info
//                                        provider,                                   //CGDataProviderRef
//                                        NULL,                                       //decode
//                                        false,                                      //should interpolate
//                                        kCGRenderingIntentDefault                   //intent
//                                        );
//
//        // Getting UIImage from CGImage
//    UIImage *image = [[UIImage alloc] initWithCGImage: imageRef];
//    CGImageRelease(imageRef);
//    CGDataProviderRelease(provider);
//    CGColorSpaceRelease(colorSpace);
//
//    return image;
//}



@end
