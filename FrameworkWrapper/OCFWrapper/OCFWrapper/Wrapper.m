//
//  Wrapper.m
//  OCFWrapper
//
//  Created by Yan Hu on 2019/11/4.
//  Copyright © 2019 yan. All rights reserved.
//

#import "Wrapper.h"
@import FWrapper;

@implementation Wrapper

- (NSString *)version {
    return FWrapper.sdkVersion;
}

@end
