//
//  Color.swift
//  xdrip
//
//  Created by Yan Hu on 2019/9/18.
//  Copyright © 2019 Johan Degraeve. All rights reserved.
//

import UIKit

//@propertyWrapper
//class ColorRGBWrapper {
//    var r: CGFloat
//    var g: CGFloat
//    var b: CGFloat
//    var alpha: CGFloat
//    var wrappedValue: UIColor {
//        get { UIColor.init(red: r / 255, green: g / 255, blue: b / 255, alpha: alpha) }
//    }
//
//    init(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat, _ alpha: CGFloat = 1) {
//        self.r = r
//        self.g = g
//        self.b = b
//        self.alpha = alpha
//    }
//}
//
//struct Color {
//    @ColorRGBWrapper(241, 73, 119)
//    static var pinkRed: UIColor
//
//    @ColorRGBWrapper(19, 25, 51)
//    static var blueBlack: UIColor
//
//    @ColorRGBWrapper(35, 40, 75)
//    static var purpleBlack: UIColor
//}

extension UIColor {
    static func rgbColor(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat, _ alpha: CGFloat = 1) -> UIColor {
        return UIColor.init(red: r / 255, green: g / 255, blue: b / 255, alpha: alpha)
    }
    
    static let pinkRed = rgbColor(241, 73, 119)
    static let modena = rgbColor(19, 25, 51)
    static let purple = rgbColor(35, 40, 75)
    static let purpleLine = rgbColor(43, 50, 100)
    static let blue = rgbColor(108, 200, 255)
    
    static let white10 = rgbColor(255, 255, 255, 0.1)
    static let white50 = rgbColor(255, 255, 255, 0.5)
    
    static let black50 = rgbColor(0, 0, 0, 0.5)
}
