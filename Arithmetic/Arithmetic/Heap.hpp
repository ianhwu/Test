//
//  Heap.hpp
//  Arithmetic
//
//  Created by Yan Hu on 2019/8/14.
//  Copyright © 2019 yan. All rights reserved.
//

#ifndef Heap_hpp
#define Heap_hpp

#include <stdio.h>
#include <iostream>
using namespace std;
template <class T>
class Heap {
protected:
    vector<T> heap;
    void filterUp();
    void filterDown(int index);
public:
    void _insert(T value);
    void deleteMax();
    void _heap(vector<T> array);
    
    void print();
    
    vector<T> sort();
};

#endif /* Heap_hpp */
