//
//  AppDelegate.swift
//  Socket
//
//  Created by Yan Hu on 2019/8/29.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit


@UIApplicationMain
class AppDelegate: UIResponder, UIApplicationDelegate {
    var window: UIWindow?

    func application(_ application: UIApplication, didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
        // Override point for customization after application launch.
        
//        DispatchQueue.global().async {
//            sleep(2)
//            let task = URLSession.shared.dataTask(with: URL.init(string: "http://localhost:9908")!, completionHandler: { (data, _, _) in
//                if let data = data {
//                    let json = try? JSONSerialization.jsonObject(with: data, options: .allowFragments)
////                    let json = String.init(data: data, encoding: .utf8)
//                    print(json)
//                }
//            })
//            task.resume()
//        }
        
        return true
    }
    
    func applicationDidEnterBackground(_ application: UIApplication) {
        // Use this method to release shared resources, save user data, invalidate timers, and store enough application state information to restore your application to its current state in case it is terminated later.
        // If your application supports background execution, this method is called instead of applicationWillTerminate: when the user quits.
        
        application.beginBackgroundTask {
            let timer = Timer.init(timeInterval: 1, repeats: true, block: { _ in
                
            })
            
            RunLoop.current.add(timer, forMode: .common)
        }
    }

    func applicationWillEnterForeground(_ application: UIApplication) {
        // Called as part of the transition from the background to the active state; here you can undo many of the changes made on entering the background.
    }

    func applicationDidBecomeActive(_ application: UIApplication) {
        // Restart any tasks that were paused (or not yet started) while the application was inactive. If the application was previously in the background, optionally refresh the user interface.
    }

    func applicationWillTerminate(_ application: UIApplication) {
        // Called when the application is about to terminate. Save data if appropriate. See also applicationDidEnterBackground:.
    }


}

