//
//  BinNode.cpp
//  Arithmetic
//
//  Created by Yan Hu on 2019/8/12.
//  Copyright © 2019 yan. All rights reserved.
//

#include "BinNode.hpp"

template <class T>
int BinNode<T>::size() {
    int s = 1;
    if (left) { s += left->size(); }
    if (right) { s += right->size(); }
    return s;
}

template <class T>
void BinNode<T>::add(BinNode<T> *child) {
    if (child->value > this->value) {
        this->right = child;
    } else {
        this->left = child;
    }
}
