//
//  Config.swift
//  Box
//
//  Created by Yan Hu on 2019/11/27.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit


/// item
let margin: CGFloat = 24
/// cell
let margin1: CGFloat = 24
/// content view
let margin2: CGFloat = 24
///
let gap: CGFloat = 16
/// icon
let iconLength: CGFloat = 24

let iconLength1: CGFloat = 34

/// items space
let space: CGFloat = 50

/// items height
let itemHeight: CGFloat = 60

/// line height
let lineHeight: CGFloat = 1

/// status bar height
var statusBarHeight: CGFloat {
    return isNotchScreen ? 44 : 20
}

let cornerRadiusLarge: CGFloat = 8
let cornerRadiusSmall: CGFloat = 4

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
