//
//  UIImage+Rotate.m
//  Foundry
//
//  Created by jonathan on 21/09/2013.
//  Copyright (c) 2013 Foundry. All rights reserved.
//
//see also http://blog.logichigh.com/2008/06/05/uiimage-fix/
//see also http://blog.9mmedia.com/?p=932
//see also http://stackoverflow.com/questions/14484816/force-uiimagepickercontroller-to-take-photo-in-portrait-orientation-dimensions-i/14491293#14491293
//see also http://stackoverflow.com/questions/13971491/captured-photo-automatically-rotated-during-upload-in-ios-6-0-or-iphone/13974815#13974815

//update 2015: `rotateToImageOrientation` code is taken verbatim from this SO answer
//http://stackoverflow.com/a/5427890/1375695

//#include <opencv2/stitching.hpp>
//#include <opencv2/imgcodecs/ios.h>

#import "UIImage+Rotate.h"
#import "UIImage+OpenCV.mm"


@implementation UIImage (Rotate)

//方差参考值
const double delta = 100;
- (BOOL)whetherBlurry {
    
    unsigned char *data;
    int height, width, step;
    
    int Iij;
    
    double Iave = 0, Idelta = 0;
    
    cv::Mat mat = [self CVMat];
    
//    if (!mat.empty()) {
//        cv::Mat gray;
//        cv::Mat outGray;
//        // 将图像转换为灰度显示
//        cv::cvtColor(mat,gray, CV_RGB2GRAY);
//        // 用openCV的Laplacian函数做灰度图做拉普拉斯计算，得到outGray
//        cv::Laplacian(gray, outGray, gray.depth());
//        //将得到的outGray转化为图片信息类IplImage
//        IplImage ipl_image(outGray);
//        
//        data   = (uchar*)ipl_image.imageData;  //图片像素信息
//        height = ipl_image.height;             //图片像素高度
//        width  = ipl_image.width;              //图片像素宽度
//        step   = ipl_image.widthStep;          //排列的图像行大小，以字节为单位
//        
//        for(int i = 0; i < height; i++) {    //求方差
//            for(int j = 0; j < width; j++) {
//                Iij = (int) data[i * width + j];
//                Idelta = Idelta + (Iij - Iave) * (Iij - Iave);
//            }
//        }
//        Idelta = Idelta / (width * height);
//        
//        std::cout<<"矩阵方差为："<<Idelta<<std::endl;
//    }
    
    return (Idelta > delta) ? YES : NO;
}


- (UIImage*)rotateExifToOrientation:(UIImageOrientation)orientation {
    return [[UIImage alloc] initWithCGImage:self.CGImage
                                      scale:self.scale
                                orientation:orientation];
}


-(UIImage*) rotateBitmapToOrientation:(UIImageOrientation)orientation
{
    UIImage* image = [self rotateExifToOrientation:orientation];
    image = [image rotateToImageOrientation];
    return image;
}




- (UIImage *)rotateToImageOrientation {

    // No-op if the orientation is already correct
    if (self.imageOrientation == UIImageOrientationUp) return self;
    
    // We need to calculate the proper transformation to make the image upright.
    // We do it in 2 steps: Rotate if Left/Right/Down, and then flip if Mirrored.
    CGAffineTransform transform = CGAffineTransformIdentity;
    
    switch (self.imageOrientation) {
        case UIImageOrientationDown:
        case UIImageOrientationDownMirrored:
            transform = CGAffineTransformTranslate(transform, self.size.width, self.size.height);
            transform = CGAffineTransformRotate(transform, M_PI);
            break;
            
        case UIImageOrientationLeft:
        case UIImageOrientationLeftMirrored:
            transform = CGAffineTransformTranslate(transform, self.size.width, 0);
            transform = CGAffineTransformRotate(transform, M_PI_2);
            break;
            
        case UIImageOrientationRight:
        case UIImageOrientationRightMirrored:
            transform = CGAffineTransformTranslate(transform, 0, self.size.height);
            transform = CGAffineTransformRotate(transform, -M_PI_2);
            break;
        case UIImageOrientationUp:
        case UIImageOrientationUpMirrored:
            break;
    }
    
    switch (self.imageOrientation) {
        case UIImageOrientationUpMirrored:
        case UIImageOrientationDownMirrored:
            transform = CGAffineTransformTranslate(transform, self.size.width, 0);
            transform = CGAffineTransformScale(transform, -1, 1);
            break;
            
        case UIImageOrientationLeftMirrored:
        case UIImageOrientationRightMirrored:
            transform = CGAffineTransformTranslate(transform, self.size.height, 0);
            transform = CGAffineTransformScale(transform, -1, 1);
            break;
        case UIImageOrientationUp:
        case UIImageOrientationDown:
        case UIImageOrientationLeft:
        case UIImageOrientationRight:
            break;
    }
    
    // Now we draw the underlying CGImage into a new context, applying the transform
    // calculated above.
    CGContextRef ctx = CGBitmapContextCreate(NULL, self.size.width, self.size.height,
                                             CGImageGetBitsPerComponent(self.CGImage), 0,
                                             CGImageGetColorSpace(self.CGImage),
                                             CGImageGetBitmapInfo(self.CGImage));
    CGContextConcatCTM(ctx, transform);
    switch (self.imageOrientation) {
        case UIImageOrientationLeft:
        case UIImageOrientationLeftMirrored:
        case UIImageOrientationRight:
        case UIImageOrientationRightMirrored:
            // Grr...
            CGContextDrawImage(ctx, CGRectMake(0,0,self.size.height,self.size.width), self.CGImage);
            break;
            
        default:
            CGContextDrawImage(ctx, CGRectMake(0,0,self.size.width,self.size.height), self.CGImage);
            break;
    }
    
    // And now we just create a new UIImage from the drawing context
    CGImageRef cgimg = CGBitmapContextCreateImage(ctx);
    UIImage *img = [UIImage imageWithCGImage:cgimg];
    CGContextRelease(ctx);
    CGImageRelease(cgimg);
    return img;
}

@end
