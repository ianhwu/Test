//
//  UIImage+Rotate.h
//  Foundry
//
//  Created by jonathan on 21/09/2013.
//  Copyright (c) 2013 Foundry. All rights reserved.
//

#ifdef __cplusplus
#import <opencv2/opencv.hpp>
#endif
#ifdef __OBJC__
    #import <UIKit/UIKit.h>
    #import <Foundation/Foundation.h>
#endif

@interface UIImage (Rotate)

//faster, alters the exif flag but doesn't change the pixel data
- (UIImage*)rotateExifToOrientation:(UIImageOrientation)orientation;

//slower, rotates the actual pixel matrix
- (UIImage*)rotateBitmapToOrientation:(UIImageOrientation)orientation;

- (UIImage*)rotateToImageOrientation;

- (BOOL)whetherBlurry;
@end
