//
//  AppDelegate.swift
//  NetworListenerTest
//
//  Created by Yan Hu on 2020/3/6.
//  Copyright © 2020 cn.com.yan. All rights reserved.
//

import Cocoa
import Network

@NSApplicationMain
class AppDelegate: NSObject, NSApplicationDelegate {

    let listener = try! NWListener.init(using: .tcp, on: 9090)
    func applicationDidFinishLaunching(_ aNotification: Notification) {
        // Insert code here to initialize your application
        listener.stateUpdateHandler = {
            state in
            print(state)
        }
        let responseData =
        """
        HTTP/1.1 200 OK
        
        <!doctype html>
        <html>
        <head>
        <title>
        Error
        </title>
        </head>
        <style>
        div {
        width: 100%;
        height: 100%;
        text-align: center;
        position: absolute;
        }
        </style>
        <body>
        <div>
        1234
        </div>
        </body>
        </html>
        """.data(using: .utf8)
        
        listener.newConnectionHandler = {
            connect in
            connect.receiveMessage { (data, context, isComplete, error) in
                if let data = data, let str = String(data: data, encoding: .utf8) {
                    print(str)
                }
            }
            connect.start(queue: .main)
            connect.send(content: responseData, completion: .contentProcessed({ (error) in
                print(error)
                connect.cancel()
            }))
        }
        listener.start(queue: .main)
    }

    var connections = [NWConnection]()
    func applicationWillTerminate(_ aNotification: Notification) {
        // Insert code here to tear down your application
    }


}

