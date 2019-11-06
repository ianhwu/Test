//
//  ShortestPath.cpp
//  Arithmetic
//
//  Created by Yan Hu on 2019/8/6.
//  Copyright © 2019 yan. All rights reserved.
//

#include "ShortestPath.hpp"

using namespace std;

/// implement with matrix
void shortPath() {
    int inf = 10e7;
    int original = 0;
    vector<vector<int>> matrix(5, vector<int>(5, inf));
    matrix[0][1] = 1;
    matrix[0][2] = 7;
    matrix[0][3] = 1;
    matrix[1][2] = 5;
    matrix[3][2] = 5;
    matrix[3][4] = 1;
    matrix[4][2] = 1;
    
    vector<int> pre(5, -1);
    vector<int> minPath(5, inf);
    minPath = matrix[original];
    vector<bool> flag(5, false);
    flag[original] = true;
    
    for (int i = 0; i < matrix.size(); i++) {
        int min = inf;
        int k = original;
        for (int j = 0; j < matrix.size(); j++) {
            if (!flag[j] && minPath[j] < min) {
                min = minPath[j];
                k = j;
            }
        }
        
        if (k == original) { break; }
        flag[k] = true;
        for (int j = 0; j < matrix.size(); j++) {
            if (!flag[j] && matrix[k][j] < inf) {
                if (minPath[j] > (minPath[k] + matrix[k][j])) {
                    minPath[j] = minPath[k] + matrix[k][j];
                    pre[j] = k;
                }
            }
        }
    }
    
    for (int i = 0; i < pre.size(); i++) {
        string temp = "无法到达";
        if (minPath[i] < inf) {
            temp = to_string(minPath[i]);
        }
        cout << "到达" << char('A' + i) << "最短距离为: " << temp << endl;
        string path = "";
        if (minPath[i] < inf) {
            int p = pre[i];
            path = to_string(i);
            while (p != -1) {
                path = to_string(p) + " -> " + path;
                p = pre[p];
            }
            path = to_string(original) + " -> " + path;
        }
        cout << "路径为: " << path << endl;
    }
}
