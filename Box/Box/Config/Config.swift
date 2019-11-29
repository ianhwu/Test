//
//  Config.swift
//  Box
//
//  Created by Yan Hu on 2019/11/27.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit


struct Config {
    /// item
    static let margin: CGFloat = 24
    /// cell
    static let margin1: CGFloat = 24
    /// content view
    static let margin2: CGFloat = 24
    ///
    static let gap: CGFloat = 16
    /// icon
    static let iconLength: CGFloat = 24

    static let iconLength1: CGFloat = 34

    /// items space
    static let space: CGFloat = 50

    /// items height
    static let itemHeight: CGFloat = 60

    /// items height
    static let cellHeight: CGFloat = 60
    
    /// items gap
    static let itemGap: CGFloat = 10

    /// line height
    static let lineHeight: CGFloat = 1

    /// status bar height
    static var statusBarHeight: CGFloat {
        return isNotchScreen ? 44 : 20
    }

    static let cornerRadiusLarge: CGFloat = 8
    static let cornerRadiusSmall: CGFloat = 4

    /// window safeAreaInsets
    static var windowSafeAreaInsets: UIEdgeInsets {
        if #available(iOS 11.0, *) {
            return (UIApplication.shared.keyWindow?.safeAreaInsets) ?? .zero
        } else {
            return .zero
        }
    }

    /// 是不是刘海屏
    static var isNotchScreen: Bool {
        if #available(iOS 11.0, *) {
            if windowSafeAreaInsets.top > 20 {
                return true
            }
        }
        return false
    }
}
