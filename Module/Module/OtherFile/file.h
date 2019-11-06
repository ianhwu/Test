//
//  file.h
//  Module
//
//  Created by Yan Hu on 2019/8/5.
//  Copyright © 2019 yan. All rights reserved.
//

#ifndef file_h
#define file_h

#include <stdio.h>
typedef void* CPPFile;

#ifdef __cplusplus
extern "C" {
#endif

// 为 c++ 提供接口
CPPFile file(void);
void printFile(CPPFile file);

#ifdef __cplusplus
}
#endif

#endif /* file_h */
