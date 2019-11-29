//
//  UIApplicationExtension.swift
//  Box
//
//  Created by Yan Hu on 2019/11/27.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit

public extension UIApplication {
    static var appVersion: String? {
        return Bundle.main.object(forInfoDictionaryKey:"CFBundleShortVersionString") as? String
    }
    
    static var buildVersion: String? {
        return Bundle.main.object(forInfoDictionaryKey:"CFBundleVersion") as? String
    }
    
    static var displayName: String? {
        return Bundle.main.object(forInfoDictionaryKey: "CFBundleDisplayName") as? String
    }
    
    static var bundleName: String? {
        return Bundle.main.object(forInfoDictionaryKey: "CFBundleName") as? String
    }
    
    static var bundleId: String? {
        return Bundle.main.object(forInfoDictionaryKey: "CFBundleIdentifier") as? String
    }
    
    static var documentsURL: URL {
        return FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).last!
    }
    
    static var documentsPath: String {
        return NSSearchPathForDirectoriesInDomains(.documentDirectory, .userDomainMask, true).first!
    }
    
    static var cachesURL: URL {
        return FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).last!
    }
    
    static var cachesPath: String {
        return NSSearchPathForDirectoriesInDomains(.cachesDirectory, .userDomainMask, true).first!
    }
    
    static var preferencesPath: String {
        return NSSearchPathForDirectoriesInDomains(.preferencePanesDirectory, .userDomainMask, true).first!
    }
    
    static var tmpPath: String {
        return NSTemporaryDirectory()
    }
    
    static var libraryURL: URL {
        return FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask).last!
    }
    
    static var libraryPath: String {
        return NSSearchPathForDirectoriesInDomains(.libraryDirectory, .userDomainMask, true).first!
    }
    
    var topViewController: UIViewController? {
        var topVc = keyWindow?.rootViewController
        while let presentedController = topVc?.presentedViewController {
            topVc = presentedController
        }
        return topViewController(of: topVc)
    }
    
    private func topViewController(of vc: UIViewController?) -> UIViewController? {
        if let nav = vc as? UINavigationController {
            return topViewController(of: nav.topViewController)
        } else if let tab = vc as? UITabBarController {
            return topViewController(of: tab.selectedViewController)
        } else {
            return vc
        }
    }
    
}

