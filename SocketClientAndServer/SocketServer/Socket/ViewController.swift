//
//  ViewController.swift
//  Socket
//
//  Created by Yan Hu on 2019/8/29.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit
import CocoaAsyncSocket

class ViewController: UIViewController, GCDAsyncSocketDelegate {
    var socket: GCDAsyncSocket!
    var newSocketArray = [GCDAsyncSocket]()
    let label = UITextView()
    override func viewDidLoad() {
        super.viewDidLoad()
        // Do any additional setup after loading the view.
        socket = GCDAsyncSocket.init(delegate: self, delegateQueue: .main)
        
        do {
            try socket.accept(onPort: 9908)
        } catch {
            print(error)
            label.text += "\nconnet error: \(error)"
        }
        
        label.frame = CGRect.init(x: 0, y: 100, width: view.frame.size.width, height: 400)
        label.text = "Server"
        label.isEditable = false
        view.addSubview(label)
    }
    
    func secureSocket(newSocket: GCDAsyncSocket) {
        // The root self-signed certificate I have created
        let path = Bundle.main.path(forResource: "yanhu", ofType: "cer")!
        let certData = NSData.init(contentsOfFile: path)!
        if let cert = SecCertificateCreateWithData(nil, certData) {
            let options : NSDictionary = [kSecImportExportPassphrase : "1234"] //客户端证书密码
            var items: CFArray?
//            let sanityChesk = SecPKCS12Import(certData, options, &items)
//            if sanityChesk != errSecSuccess {
//                return
//            }
//
//            let identityDict = CFArrayGetValueAtIndex(items, 0) as! CFDictionary
//            let ident = CFDictionaryGetValue(identityDict, kSecImportItemIdentity as? UnsafePointer)
            
            let sslSettings : [String: Any] = [
                GCDAsyncSocketManuallyEvaluateTrust: NSNumber.init(value: true),
                // We are an SSL/TLS server
                kCFStreamSSLIsServer as String: NSNumber.init(value: true),
                kCFStreamSSLCertificates as String: [cert],
                kCFStreamSSLValidatesCertificateChain as String: NSNumber.init(value: false),
                kCFStreamSSLPeerName as String: NSNull.init()
            ]
            
            newSocket.startTLS((sslSettings as! [String : NSObject]))
        }
    }
    
    func socket(_ sock: GCDAsyncSocket, didAcceptNewSocket newSocket: GCDAsyncSocket) {
        print(newSocket.connectedHost!)
//        secureSocket(newSocket: newSocket)
        label.text += "\nconnet to: \(newSocket.connectedHost ?? "")"
        newSocketArray.append(newSocket)
        newSocket.readData(withTimeout: -1, tag: 0)
    }
    
    func socket(_ sock: GCDAsyncSocket, didRead data: Data, withTag tag: Int) {
        if let str = String.init(data: data, encoding: .utf8) {
            print(str)
            label.text += "\nreceive: \(str)"
            sock.readData(withTimeout: -1, tag: 0)
//            if str.contains("HTTP") {
                write()
//            }
        }
    }
    
    func write() {
        if let data =
"""
HTTP/1.1 200 OK

{"name": "lao wang", "age": 30, "gender": "male"}
""".data(using: .utf8) {
            for socket in newSocketArray {
                socket.write(data, withTimeout: -1, tag: 0)
//                socket.disconnect()
            }
        }
    }
    
    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        write()
    }
    
    func socket(_ sock: GCDAsyncSocket, didConnectToHost host: String, port: UInt16) {
//        if let p12 = try? Data.init(contentsOf: URL.init(fileURLWithPath: Bundle.main.path(forResource: "yanhu", ofType: "p12")!)) {
//            var json = [String: Any]()
//            let  SecPKCS12Import(<#T##pkcs12_data: CFData##CFData#>, <#T##options: CFDictionary##CFDictionary#>, <#T##items: UnsafeMutablePointer<CFArray?>##UnsafeMutablePointer<CFArray?>#>)
//        }
        
    }
    
    func socket(_ sock: GCDAsyncSocket, didReceive trust: SecTrust, completionHandler: @escaping (Bool) -> Void) {
        completionHandler(true)
    }
    
    func socketDidSecure(_ sock: GCDAsyncSocket) {
        
    }
}
