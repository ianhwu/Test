//
//  main.cpp
//  Arithmetic
//
//  Created by Yan Hu on 2019/8/6.
//  Copyright © 2019 yan. All rights reserved.
//

#include <iostream>
#include "quickSort.hpp"
#include "HeapSort.hpp"
#include "ShortestPath.hpp"

#include "Tree.cpp"
#include "Heap.cpp"
/*
 #include "Tree.cpp"
 #include "Heap.cpp"
 becouse of template
 https://stackoverflow.com/questions/34043432/c-template-header-cpp-separation-solution-including-cpp-into-h-is-not-wor
 Unlike member functions of ordinary classes, member functions of template classes cannot be compiled separately and linked into the executable. The members of a template must be visible to the compiler at the point where they're used. That's what all that nonsensical include stuff in that horrible article is about.
 
 The simplest way to do this is to put the definitions directly into the template definition:
 
 #ifndef TEST_H
 #define TEST_H
 
 template <class Ty>
 class test {
 public:
 void f() {}
};
#endif
This has the drawback that larger classes become unreadable (cf. Java). So the next step is to move the definitions outside the template, but keep them in the header:

#ifndef TEST_H
#define TEST_H

template <class Ty>
class test {
public:
    void f();
};

template <class Ty>
void test<Ty>::f() {}

#endif
Many people feel that that's still too cluttered, and want to put the definitions into a separate file. That's okay, too, but you have to make sure that that separate file gets included whenever the original header is used:

#ifndef TEST_H
#define TEST_H

template <class Ty>
class test {
public:
    void f();
};

#include "test.imp"

#endif
This is the file "test.imp":

#ifndef TEST_IMP
#define TEST_IMP

template <class Ty>
void test<Ty>::f() {}

#endif
Note that "test.imp" is really a header file, so it gets into your code through the #include "test.imp" directive in test.h. It cannot be compiled separately, so should not be named with a .cpp extension, which would, at best, be misleading.
 */

int main(int argc, const char * argv[]) {
    // insert code here...

    SplayTree<int> tree = SplayTree<int>(200);
    tree._insert(190);
    tree._insert(180);
    tree._insert(170);
    tree._insert(160);
    tree._insert(150);
    tree._insert(140);
    tree._insert(130);
    tree._insert(120);
    tree._insert(110);
    tree._insert(100);
    tree._search(100);
    tree.printTree();
    
    
    tree._search(200);
    tree.printTree();
    
    tree._delete(170);
    tree.printTree();
    
//    tree._search(180);
//    tree.printTree();
//    
//    tree._search(140);
//    tree.printTree();
//    
//    tree._search(180);
//    tree.printTree();
    
    return 0;
}
