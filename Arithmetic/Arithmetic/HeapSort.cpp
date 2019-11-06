//
//  HeapSort.cpp
//  Arithmetic
//
//  Created by Yan Hu on 2019/8/6.
//  Copyright © 2019 yan. All rights reserved.
//

#include "HeapSort.hpp"
#include <iostream>

vector<int> heapSort(vector<int> a) {
    vector<int> s;
    heap(a);
    int size = int(a.size());
    for (int i = 0; i < size; i++) {
        s.push_back(a[0]);
        if (i != size - 1) {
            delMax(a);
        }
    }
    return s;
}

void delMax(vector<int> &a) {
    swap(a[0], a[a.size() - 1]);
    a.erase(a.end() - 1);
    filterDown(a, 0);
}

void heap(vector<int> &a) {
    int index = int(a.size()) / 2 - 1;
    for (int i = index; i >= 0; i--) {
        filterDown(a, i);
    }
}

void filterDown(vector<int> &a, int index) {
    bool end = false;
    int i = index;
    while (!end) {
        end = true;
        if (a.size() > i * 2 + 1) {
            int max = i * 2 + 1;
            if (a.size() > (i + 1) * 2) {
                if (a[max] < a[(i + 1) * 2]) {
                    max = (i + 1) * 2;
                }
            }
            
            if (a[i] < a[max]) {
                swap(a[i], a[max]);
                i = max;
                end = false;
            }
        }
    }
}
