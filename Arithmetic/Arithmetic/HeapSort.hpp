//
//  HeapSort.hpp
//  Arithmetic
//
//  Created by Yan Hu on 2019/8/6.
//  Copyright © 2019 yan. All rights reserved.
//

#ifndef HeapSort_hpp
#define HeapSort_hpp

#include <stdio.h>
#include <vector>
using namespace std;

vector<int> heapSort(vector<int> a);
void filterDown(vector<int> &a, int index);
void heap(vector<int> &a);
void delMax(vector<int> &a);

#endif /* HeapSort_hpp */
