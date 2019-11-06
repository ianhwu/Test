//
//  ViewController.swift
//  Socket
//
//  Created by Yan Hu on 2019/8/29.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit
import CocoaAsyncSocket

class ViewController: UIViewController, GCDAsyncSocketDelegate, UITextViewDelegate {
    var socket: GCDAsyncSocket!
    let label = UITextView()
    let textView = UITextView()
    override func viewDidLoad() {
        super.viewDidLoad()
        // Do any additional setup after loading the view.
        socket = GCDAsyncSocket.init(delegate: self, delegateQueue: .main)
        do {
            try socket.connect(toHost: "0.0.0.0", onPort: 9908, viaInterface: nil, withTimeout: -1)
        } catch {
            print("\n error: \(error))")
            label.text += "\n error: \(error))"
        }
        
        label.isEditable = false
        label.frame = CGRect.init(x: 0, y: 30, width: view.frame.size.width, height: 250)
        label.text = "Client"
        view.addSubview(label)
        
        textView.frame = CGRect.init(x: 0, y: 300, width: view.frame.size.width, height: 200)
        textView.backgroundColor = .gray
        textView.delegate = self
        view.addSubview(textView)
    }
    
    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        textView.resignFirstResponder()
    }
    
    func textViewDidChange(_ textView: UITextView) {
        if let data = textView.text.data(using: .utf8) {
            socket.write(data, withTimeout: -1, tag: 0)
        }
    }
    
    func socket(_ sock: GCDAsyncSocket, didRead data: Data, withTag tag: Int) {
        if let str = String.init(data: data, encoding: .utf8) {
            print(str)
            label.text += "\nreceive: \(str)"
            sock.readData(withTimeout: -1, tag: 0)
        }
    }
    
    func socket(_ sock: GCDAsyncSocket, didAcceptNewSocket newSocket: GCDAsyncSocket) {
        print("connetted: " ,newSocket.connectedHost)
        label.text += "\nconnetted: \(newSocket.connectedHost)"
    }
    
    func socket(_ sock: GCDAsyncSocket, didConnectToHost host: String, port: UInt16) {
        print(host, port)
        sock.readData(withTimeout: -1, tag: 0)
        label.text += "\nconnect to\(host):\(port)"
    }
    
    
    func socketDidDisconnect(_ sock: GCDAsyncSocket, withError err: Error?) {
        print("disconnect: ", err)
        label.text += "\ndisconnect: \(err)"
    }
}
