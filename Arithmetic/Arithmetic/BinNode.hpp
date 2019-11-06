//
//  BinNode.hpp
//  Arithmetic
//
//  Created by Yan Hu on 2019/8/12.
//  Copyright © 2019 yan. All rights reserved.
//

#ifndef BinNode_hpp
#define BinNode_hpp

#include <stdio.h>


template <class T>
class BinNode {
public:
    BinNode *left;
    BinNode *right;
    BinNode *parent;
    
    T value;
    int size();
    int height;
    
    void add(BinNode *child);
    
    bool operator<(BinNode const& node) { return value < node.value; }
    bool operator=(BinNode const& node) { return value = node.value; }
    bool operator>(BinNode const& node) { return value > node.value; }
};

#endif /* BinNode_hpp */
