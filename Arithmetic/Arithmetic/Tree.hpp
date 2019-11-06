//
//  Tree.hpp
//  Arithmetic
//
//  Created by Yan Hu on 2019/8/12.
//  Copyright © 2019 yan. All rights reserved.
//

#ifndef Tree_hpp
#define Tree_hpp

#include <stdio.h>
#include <iostream>
#include "BinNode.cpp"
#include <queue>

template <class T>
class Tree {
    typedef class BinNode<T> BinNode;
protected:
    BinNode* _root = new BinNode();
public:
    void travLevel();
};

template <class T>
class BSTree: Tree<T> {
    typedef class BinNode<T> BinNode;
protected:
    BinNode* _root = Tree<T>::_root;
    int _size;
    /// 应该找到的点的父亲
    BinNode* _hot;
    
    bool _deleteLR(BinNode *node);
    
    BinNode* updateHeight(BinNode *node);
    void updateHeightAbout(BinNode *node);
    void deleteCurrent(BinNode *current, BinNode *child);
    void _insert(BinNode* node, T value);
    void _delete(BinNode* node);
    void zig(BinNode* node); // 顺时针旋转
    void zag(BinNode* node); // 逆时针旋转
public:
    BSTree(T value);
    int height(BinNode *node);
    int size() { return _size; }
    
    BinNode* root() { return this->_root; }
    BinNode* _search(T value);
    BinNode* _insert(T value);
    BinNode* _delete(T value);
    // 直接后继
    BinNode* directSuccessor(BinNode *node);
    
    void printTree();
};

// Splay 伸展树, 不对树高进行处理
template <class T>
class SplayTree: BSTree<T> {
    typedef class BinNode<T> BinNode;
private:
    void splay(BinNode *node);
    void splayZig(BinNode* node); // 顺时针旋转
    void splayZag(BinNode* node); // 逆时针旋转
    
protected:
    int _size;
public:
    SplayTree(T value):BSTree<T>(value) {};
    int size() { return BSTree<T>::size(); }
    BinNode* root() { return BSTree<T>::root(); }
    
    BinNode* _insert(T value);
    BinNode* _search(T value);
    BinNode* _delete(T value);
    
    void printTree() { BSTree<T>::printTree(); }
};

// Todo:
template <class T>
class AVLTree: BSTree<T> {
    typedef class BinNode<T> BinNode;
protected:
    bool blance(BinNode *node);
public:
    BinNode* insert(T value);
};

#endif /* Tree_hpp */


