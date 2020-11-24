//
//  AppDelegate.swift
//  LeaksTest
//
//  Created by Yan Hu on 2019/12/23.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit

class Test: NSObject {
    deinit {
        print("deinit")
    }
}

@UIApplicationMain
class AppDelegate: UIResponder, UIApplicationDelegate {



    func application(_ application: UIApplication, didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
        // Override point for customization after application launch.
        
        let t = Test()
        print(t)
        let b = {
            [weak t] in
            guard let t = t else { return }
            let c = {
                print("c end", t)
            }
            
            DispatchQueue.global().async {
                sleep(3)
                c()
            }
        }
        b()
        
        
        return true
    }

    // MARK: UISceneSession Lifecycle

    func application(_ application: UIApplication, configurationForConnecting connectingSceneSession: UISceneSession, options: UIScene.ConnectionOptions) -> UISceneConfiguration {
        // Called when a new scene session is being created.
        // Use this method to select a configuration to create the new scene with.
        return UISceneConfiguration(name: "Default Configuration", sessionRole: connectingSceneSession.role)
    }

    func application(_ application: UIApplication, didDiscardSceneSessions sceneSessions: Set<UISceneSession>) {
        // Called when the user discards a scene session.
        // If any sessions were discarded while the application was not running, this will be called shortly after application:didFinishLaunchingWithOptions.
        // Use this method to release any resources that were specific to the discarded scenes, as they will not return.
    }


}

