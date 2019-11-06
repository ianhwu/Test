//
//  file3.c
//  Module
//
//  Created by Yan Hu on 2019/10/14.
//  Copyright © 2019 yan. All rights reserved.
//

//我也不知道为什么, 同时存在大于等于两个 oc 的.h和大于等于两个 c 的 .h, 就出现编译错误, 可以完全使用其他方式实现, 在 stackoverflow 提了个问题, 可以持续关注一下 https://stackoverflow.com/questions/58372027/clang-module-failed-when-it-contained-two-c-files-and-two-oc-files

#include "filebbb.h"

void file3Run(void) {
    printf("fileb run\n");
}
