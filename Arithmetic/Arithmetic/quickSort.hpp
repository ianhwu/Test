//
//  QS.hpp
//  Arithmetic
//
//  Created by Yan Hu on 2019/7/25.
//  Copyright © 2019 yan. All rights reserved.
//

#ifndef QS_hpp
#define QS_hpp

#include <stdio.h>
#include <iostream>


#endif /* QS_hpp */

template <class T>
struct Node {
    Node *succ;
    Node *next;
    
    T value;
};
template <class T>
void quickSort(Node<T> *first);
