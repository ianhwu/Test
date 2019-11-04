//
//  AppDelegate.swift
//  SwiftTest
//
//  Created by Yan Hu on 2019/10/16.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit
import OCFWrapper

@UIApplicationMain
class AppDelegate: UIResponder, UIApplicationDelegate {
    var window: UIWindow?
    func application(_ application: UIApplication, didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
        // Override point for customization after application launch.
        window?.rootViewController = ViewController()
        
        print(Wrapper().version())
        
        return true
    }

}

