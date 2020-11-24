//
//  CVWrapper.h
//  CVOpenTemplate
//
//  Created by Washe on 02/01/2013.
//  Copyright (c) 2013 foundry. All rights reserved.
//

#ifdef __cplusplus
#import <opencv2/opencv.hpp>
#endif
#ifdef __OBJC__
    #import <UIKit/UIKit.h>
    #import <Foundation/Foundation.h>
#endif

NS_ASSUME_NONNULL_BEGIN
@interface CVWrapper : NSObject

+ (UIImage *)processImageWithOpenCV:(UIImage *)inputImage;

+ (UIImage *)processWithOpenCVImage1:(UIImage *)inputImage1 image2:(UIImage *)inputImage2;

+ (UIImage *)processWithArray:(NSArray<UIImage *> *)imageArray;

+ (void)run;

+ (UIImageView *)cameraView;

@end

NS_ASSUME_NONNULL_END

