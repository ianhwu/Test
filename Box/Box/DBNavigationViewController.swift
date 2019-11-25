//
//  DBNavigationViewController.swift
//  Box
//
//  Created by Yan Hu on 2019/11/25.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit
import SnapKit

/// status bar height
var statusBarHeight: CGFloat {
    return isNotchScreen ? 44 : 20
}

/// window safeAreaInsets
var windowSafeAreaInsets: UIEdgeInsets {
    if #available(iOS 11.0, *) {
        return (UIApplication.shared.keyWindow?.safeAreaInsets)!
    } else {
        return .zero
    }
}

/// 是不是刘海屏
var isNotchScreen: Bool {
    if #available(iOS 11.0, *) {
        if windowSafeAreaInsets.top > 20 {
            return true
        }
    }
    return false
}

class DBNavigationViewController: UINavigationController, UIGestureRecognizerDelegate {

    override func viewDidLoad() {
        super.viewDidLoad()

        // Do any additional setup after loading the view.
        navigationBar.isHidden = true
        interactivePopGestureRecognizer?.delegate = self
    }

    /*
    // MARK: - Navigation

    // In a storyboard-based application, you will often want to do a little preparation before navigation
    override func prepare(for segue: UIStoryboardSegue, sender: Any?) {
        // Get the new view controller using segue.destination.
        // Pass the selected object to the new view controller.
    }
    */

}
