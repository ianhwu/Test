//
//  file_plus.cpp
//  Module
//
//  Created by Yan Hu on 2019/8/5.
//  Copyright © 2019 yan. All rights reserved.
//

#include "file_plus.hpp"

using namespace std;

// 方法的具体实现
CPPFile file(void) {
    return new File();
}

void printFile(CPPFile file) {
    File *temp = (File *)file;
    cout << temp->text << endl;
}
