//
//  ViewController.swift
//  Socket
//
//  Created by Yan Hu on 2019/8/29.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit
import CocoaAsyncSocket

class ViewController: UIViewController, GCDAsyncSocketDelegate, URLSessionDelegate {
    var socket: GCDAsyncSocket!
    var newSocketArray = [GCDAsyncSocket]()
    let label = UITextView()
    
    func importPKCS12(data: Data, passphrase: String) -> SecIdentity? {
        var query = [String: Any]()
      query[kSecImportExportPassphrase as String] = passphrase as NSString

      // Import the data
      var importResult: CFArray?
      let status = withUnsafeMutablePointer(to: &importResult) { SecPKCS12Import(data as NSData, query as CFDictionary, $0) }
      guard status == errSecSuccess else { return nil }

      // The result is an array of dictionaries, we are looking for the one that contains the identity
      let importArray = importResult as? [[NSString: AnyObject]]
      let importIdentity = importArray?.compactMap { dict in dict[kSecImportItemIdentity as NSString] }.first

      // Let's double check that we have a result and that it is a SecIdentity
      guard let rawResult = importIdentity, CFGetTypeID(rawResult) == SecIdentityGetTypeID() else { return nil }
      let result = rawResult as! SecIdentity

      return result
    }
    
    override func viewDidLoad() {
        super.viewDidLoad()
        // Do any additional setup after loading the view.
        socket = GCDAsyncSocket.init(delegate: self, delegateQueue: .main)
        do {
            try socket.accept(onPort: 8889)
        } catch {
            print(error)
            label.text += "\nconnet error: \(error)"
        }
        
        label.frame = CGRect.init(x: 0, y: 100, width: view.frame.size.width, height: 400)
        label.text = "Server"
        label.isEditable = false
        view.addSubview(label)
    }
    
    var session: URLSession!
    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        session = URLSession.init(configuration: .default, delegate: self, delegateQueue: .main)
        let task = session.dataTask(with: URL.init(string: "http://localhost:8889")!) { (data, response, error) in
            print(String(data: data!, encoding: .utf8)!)
        }
        
        task.resume()
    }
    
    func urlSession(_ session: URLSession, didReceive challenge: URLAuthenticationChallenge, completionHandler: @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void) {
        let card = URLCredential.init(trust: challenge.protectionSpace.serverTrust!)
        completionHandler(.useCredential, card)
    }
    func socket(_ sock: GCDAsyncSocket, didAcceptNewSocket newSocket: GCDAsyncSocket) {
        print(newSocket.connectedHost!)
//        label.text += "\nconnet to: \(newSocket.connectedHost ?? "")"
//
//        let caCertificateURL = Bundle.main.url(forResource: "server", withExtension: "der")!
//        let derData = try! Data.init(contentsOf: caCertificateURL)
//        let identityURL = Bundle.main.url(forResource: "server", withExtension: "p12")!
//        let idenData = try! Data.init(contentsOf: identityURL)
//        let identity = importPKCS12(data: idenData, passphrase: "diabox")!
//
//        guard let certificate = SecCertificateCreateWithData(kCFAllocatorDefault, derData as CFData) else { return }
//        let settings = [kCFStreamSSLIsServer as String: NSNumber.init(value: true),
//                        kCFStreamSSLCertificates as String: NSArray(array: [identity, certificate])
//        ]
//        newSocket.startTLS(settings)
        newSocketArray.append(newSocket)
        newSocket.readData(withTimeout: 60, tag: 0)
    }
    
    func socket(_ sock: GCDAsyncSocket, didRead data: Data, withTag tag: Int) {
        if let str = String.init(data: data, encoding: .utf8) {
            print(str)
            label.text += "\nreceive: \(str)"
            sock.readData(withTimeout: -1, tag: 0)
            if str.contains("HTTP") {
                write(sock: sock)
            }
        }
    }
    
    func socket(_ sock: GCDAsyncSocket, didReceive trust: SecTrust, completionHandler: @escaping (Bool) -> Void) {
        completionHandler(true)
    }
    
    func socketDidDisconnect(_ sock: GCDAsyncSocket, withError err: Error?) {
        for i in 0 ..< newSocketArray.count {
            if newSocketArray[i] == sock {
                newSocketArray.remove(at: i)
                break
            }
        }
    }
    
    func socket(_ sock: GCDAsyncSocket, didWriteDataWithTag tag: Int) {
        
    }
    
    func socketDidSecure(_ sock: GCDAsyncSocket) {
        
    }
    
    func write(sock: GCDAsyncSocket) {
        let img = UIImage.init(named: "face.jpg")
        if let imgData = img?.jpegData(compressionQuality: 1) {
            let res =
            """
            HTTP/1.1 200 OK
            Accept-Ranges: bytes
            Content-Type: image/jpeg
            Content-Length: \(imgData.count)
            """
            
            if let data = res.data(using: .utf8) {
                sock.write(data, withTimeout: -1, tag: 0)
                sock.write(imgData, withTimeout: -1, tag: 0)
                sock.disconnect()
            }
        }
    }
}
