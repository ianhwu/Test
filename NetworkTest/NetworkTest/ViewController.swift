//
//  ViewController.swift
//  NetworkTest
//
//  Created by Yan Hu on 2020/3/6.
//  Copyright © 2020 cn.com.yan. All rights reserved.
//

import UIKit
import Network

class ViewController: UIViewController {

    let connect = NWConnection.init(host: "localhost", port: 17580, using: .tcp)
    override func viewDidLoad() {
        super.viewDidLoad()
        // Do any additional setup after loading the view.
        view.backgroundColor = .white
        connect.receiveMessage { (data, context, isA, error) in
            if let data = data, let str = String.init(data: data, encoding: .utf8) {
                print(str)
            }
        }
        
        connect.stateUpdateHandler = {
            state in
            print(state)
        }
        
        connect.start(queue: .main)
        connect.send(content: "GET /?count=20 HTTP/1.1\r\nHost: localhost:17580\r\nUpgrade-Insecure-Requests: 1\r\nAccept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\nUser-Agent: Mozilla/5.0 (iPhone; CPU iPhone OS 12_4 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/12.1.2 Mobile/15E148 Safari/604.1\r\nAccept-Language: zh-cn\r\nAccept-Encoding: gzip, deflate\r\nConnection: keep-alive\r\n\r\n".data(using: .utf8), completion: .contentProcessed({ (error) in
            print(error)
        }))
    }
}

