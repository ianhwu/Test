//
//  Tree.cpp
//  Arithmetic
//
//  Created by Yan Hu on 2019/8/12.
//  Copyright © 2019 yan. All rights reserved.
//

#include "Tree.hpp"
using namespace std;

/****************** BST *******************/
template <class T>
BSTree<T>::BSTree(T value) {
    root()->value = value;
    root()->height = 0;
    _size = 1;
};

template <class T>
BinNode<T>* BSTree<T>::directSuccessor(BinNode *node) {
    BinNode *succ = node->right;
    bool end = false;
    while (!end) {
        if (succ->left != nullptr) {
            succ = succ->left;
        } else {
            end = true;
        }
    }
    return succ;
};

template <class T>
BinNode<T>* BSTree<T>::_search(T value) {
    BinNode *finalNode = nullptr;
    BinNode *node = root();
    bool end = false;
    _hot = node;
    while (!end && node != nullptr) {
        if (node->value == value) {
            finalNode = node;
            end = true;
        } else {
            if (node->value > value) {
                _hot = node;
                node = node->left;
            } else {
                _hot = node;
                node = node->right;
            }
        }
    }
    return finalNode;
};

template <class T>
void BSTree<T>::_insert(BinNode* node, T value) {
    node->value = value;
    if (_hot->value > value) {
        _hot->left = node;
    } else {
        _hot->right = node;
    }
    node->parent = _hot;
    updateHeightAbout(node);
    _size += 1;
};

template <class T>
BinNode<T>* BSTree<T>::_insert(T value) {
    BinNode *node = _search(value);
    if (node == nullptr) {
        node = new BinNode();
        _insert(node, value);
        return node;
    } else {
        return node;
    }
};

template <class T>
int BSTree<T>::height(BinNode *node) {
    return node ? node->height : -1;
};

template <class T>
BinNode<T>* BSTree<T>::updateHeight(BinNode *node) {
    node->height = 1 + max(height(node->left), height(node->right));
    return node;
};

template <class T>
void BSTree<T>::updateHeightAbout(BinNode *node) {
    while (node) {
        int h = height(node);
        updateHeight(node);
        if (node->height == h && h != 0) { break; }
        node = node->parent;
    }
}

template <class T>
void BSTree<T>::deleteCurrent(BinNode *current, BinNode *child) {
    if (current->value == current->parent->left->value) {
        current->parent->left = child;
    } else {
        current->parent->right = child;
    }
}

template <class T>
bool BSTree<T>::_deleteLR(BinNode *node) {
    if (node->left == nullptr) {
        deleteCurrent(node, node->right);
        return true;
    } else if (node->right == nullptr) {
        deleteCurrent(node, node->left);
        return true;
    }
    return false;
}

template <class T>
void BSTree<T>::_delete(BinNode* node) {
    if (!_deleteLR(node)) {
        // 如果左右孩子都不为空, 找到直接后继, 交换, 再删除
        BinNode *succ = directSuccessor(node);
        swap(node->value, succ->value);
        _deleteLR(succ);
    }
};

template <class T>
BinNode<T>* BSTree<T>::_delete(T value) {
    BinNode *node = _search(value);
    if (node != nullptr) {
        _delete(node);
        _size -= 1;
        if (_size == 0) {
            _root = nullptr;
        }
        updateHeightAbout(node);
    }
    return node;
};

// -->
template <class T>
void BSTree<T>::zig(BinNode* node) {
    BinNode* parent = node->parent;
    BinNode* gParent = parent->parent;
    if (parent != nullptr) {
        if (gParent != nullptr) {
            if (gParent->left == parent) {
                gParent->left = node;
            } else {
                gParent->right = node;
            }
        }
        node->parent = gParent;
        parent->left = node->right;
        if (node->right != nullptr) {
            node->right->parent = parent;
        }
        parent->parent = node;
        node->right = parent;
    }
}

// <--
template <class T>
void BSTree<T>::zag(BinNode* node) {
    BinNode* parent = node->parent;
    BinNode* gParent = parent->parent;
    if (parent != nullptr) {
        if (gParent != nullptr) {
            if (gParent->left == parent) {
                gParent->left = node;
            } else {
                gParent->right = node;
            }
        }
        node->parent = gParent;
        parent->right = node->left;
        if (node->left != nullptr) {
            node->left->parent = parent;
        }
        parent->parent = node;
        node->left = parent;
    }
}

template <class T>
void print(BinNode<T> *node) {
    if (!node) { return; }
    if (node->left != nullptr) {
        cout << "current: " << node->value;
        cout <<  " left: " << node->left->value << endl;
        print(node->left);
    }
    
    if (node->right != nullptr) {
        cout << "current: " << node->value;
        cout << " right: " << node->right->value << endl;
        print(node->right);
    }
}

template <class T>
void BSTree<T>::printTree() {
    cout << "Print Start" << endl;
    print(root());
    cout << "Print End" << endl;
}

/****************** Splay *******************/

// -->
template <class T>
void SplayTree<T>::splayZig(BinNode* node) {
    BinNode* parent = node->parent;
    if (parent != nullptr) {
        BSTree<T>::zig(parent);
        BSTree<T>::zig(node);
    }
}

// <--
template <class T>
void SplayTree<T>::splayZag(BinNode* node) {
    BinNode* parent = node->parent;
    if (parent != nullptr) {
        BSTree<T>::zag(parent);
        BSTree<T>::zag(node);
    }
}

template <class T>
void SplayTree<T>::splay(BinNode *node) {
    BinNode *parent = node->parent;
    while (parent != nullptr) {
        BinNode *gParent = parent ->parent;
        if (gParent != nullptr) {
            if (gParent->left == parent && parent->left == node) {
                splayZig(node);
            } else if (gParent->right == parent && parent->right == node) {
                splayZag(node);
            } else if (parent->right == node) {
                BSTree<T>::zag(node);
                BSTree<T>::zig(node);
            } else if (parent->left == node) {
                BSTree<T>::zig(node);
                BSTree<T>::zag(node);
            }
        } else {
            if (parent->right == node) {
                BSTree<T>::zag(node);
            } else if (parent->left == node) {
                BSTree<T>::zig(node);
            }
        }
        parent = node->parent;
    }
}

template <class T>
BinNode<T>* SplayTree<T>::_search(T value) {
    BinNode* node = BSTree<T>::_search(value);
    BinNode* sNode = node ? node : BSTree<T>::_hot;
    splay(sNode);
    BSTree<T>::_root = sNode;
    return node;
}

template <class T>
BinNode<T>* SplayTree<T>::_insert(T value) {
    BinNode *node = _search(value);
    if (!node) {
        node = BSTree<T>::_hot;
        BinNode* newNode = new BinNode();
        newNode->value = value;
        if (node->value > value) {
            newNode->left = node->left;
            node->left = nullptr;
            newNode->right = node;
        } else {
            newNode->right = node->right;
            node->right = nullptr;
            newNode->left = node;
        }
        node->parent = newNode;
        BSTree<T>::_root = newNode;
        _size += 1;
        return newNode;
    } else {
        return node;
    }
}

template <class T>
BinNode<T>* SplayTree<T>::_delete(T value) {
    BinNode *node = _search(value);
    if (node) {
        BSTree<T>::_delete(node);
        _size -= 1;
        if (_size == 0) {
            BSTree<T>::_root = nullptr;
        }
    }
    return node;
}

/****************** AVL *******************/

template <class T>
BinNode<T>* AVLTree<T>::insert(T value) {
    BinNode *node = this->_search(value);
    if (node != nullptr) { return node; }
    node = new BinNode();
    this->_insert(node, value);
}
