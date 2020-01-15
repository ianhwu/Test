//
//  Heap.cpp
//  Arithmetic
//
//  Created by Yan Hu on 2019/8/14.
//  Copyright © 2019 yan. All rights reserved.
//

#include "Heap.hpp"

template <class T>
void Heap<T>::_heap(vector<T> array) {
    heap = array;
    int index = int(heap.size()) / 2 - 1;
    for (int i = index; i >= 0; i--) {
        filterDown(i);
    }
}

template <class T>
void Heap<T>::print() {
    for (int i = 0; i < heap.size(); i++) {
        cout << heap[i] << endl;
    }
    
    cout << "end" << endl;
}

template <class T>
void Heap<T>::_insert(T value) {
    heap.push_back(value);
    filterUp();
}

template <class T>
void Heap<T>::filterUp() {
    int index = int(heap.size() - 1);
    bool end = false;
    T value = heap[index];
    while (!end) {
        int offset = index % 2 == 0 ? 1 : 0;
        if (heap[index / 2 - offset] < value && index != 0) {
            heap[index] = heap[index / 2 - offset];
            index = index / 2 - offset;
        } else {
            heap[index] = value;
            end = true;
        }
    }
}

template <class T>
void Heap<T>::filterDown(int index) {
    bool end = false;
    int i = index;
    while (!end) {
        end = true;
        if (heap.size() > i * 2 + 1) {
            int max = i * 2 + 1;
            if (heap.size() > (i + 1) * 2) {
                if (heap[max] < heap[(i + 1) * 2]) {
                    max = (i + 1) * 2;
                }
            }
            
            if (heap[i] < heap[max]) {
                swap(heap[i], heap[max]);
                i = max;
                end = false;
            }
        }
    }
}

template <class T>
void Heap<T>::deleteMax() {
    swap(heap[0], heap[heap.size() - 1]);
    heap.erase(heap.end() - 1);
    filterDown(0);
}

template <class T>
vector<T> Heap<T>::sort() {
    Heap copyHeap = Heap(*this);
    int size = int(copyHeap.heap.size());
    vector<T> s(size);
    for (int i = 0; i < size; i++) {
        s[i] = copyHeap.heap[0];
        if (i != size - 1) {
            copyHeap.deleteMax();
        }
    }
    return s;
}
