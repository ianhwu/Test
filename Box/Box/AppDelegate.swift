//
//  AppDelegate.swift
//  Box
//
//  Created by Yan Hu on 2019/11/25.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit

@UIApplicationMain
class AppDelegate: UIResponder, UIApplicationDelegate {
    var window: UIWindow?
    func application(_ application: UIApplication, didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
        // Override point for customization after application launch.
        window = UIWindow.init(frame: UIScreen.main.bounds)
        window?.makeKeyAndVisible()
        let nav = DBNavigationViewController.init(rootViewController: ViewController())
        window?.rootViewController = nav
        
        return true
    }
}
