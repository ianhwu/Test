//
//  WebView.swift
//  SwiftUI+CoreData
//
//  Created by Yan Hu on 2020/5/25.
//  Copyright © 2020 cn.com.yan. All rights reserved.
//

import Foundation
import Combine
import WebKit
import SwiftUI

class ViewModel: ObservableObject {
    var webViewNavigationPublisher = PassthroughSubject<WebViewNavigation, Never>()
    var showLoader = PassthroughSubject<Bool, Never>()
    var valuePublisher = PassthroughSubject<[String: Any], Never>()
    @Published var a: String?
    
}

enum WebViewNavigation {
    case backward, forward
}

enum WebUrlType {
    case localUrl, publicUrl
}

struct WebView: UIViewRepresentable {
    var urlType: WebUrlType
    // Viewmodel object
    @ObservedObject var viewModel: ViewModel
    // Make a coordinator to co-ordinate with WKWebView's default delegate functions
    func makeCoordinator() -> Coordinator {
        Coordinator(self)
    }
    
    func makeUIView(context: Context) -> WKWebView {
        // Enable javascript in WKWebView to interact with the web app
        let preferences = WKPreferences()
        preferences.javaScriptEnabled = true
        
        let configuration = WKWebViewConfiguration()
        // Here "iOSNative" is our interface name that we pushed to the website that is being loaded
        configuration.userContentController.add(self.makeCoordinator(), name: "iOSNative")
        configuration.preferences = preferences
        
        let webView = WKWebView(frame: CGRect.zero, configuration: configuration)
        webView.navigationDelegate = context.coordinator
        webView.allowsBackForwardNavigationGestures = true
        
        return webView
    }
    
    func updateUIView(_ webView: WKWebView, context: Context) {
        if urlType == .localUrl {
            // Load local website
            if let url = Bundle.main.url(forResource: "local", withExtension: "html") {
                webView.loadFileURL(url, allowingReadAccessTo: url.deletingLastPathComponent())
            }
        } else if urlType == .publicUrl {
            // Load a public website
            if let url = URL(string: "https://www.example.com") {
                webView.load(URLRequest(url: url))
            }
        }
    }
    
    class Coordinator : NSObject, WKNavigationDelegate {
        var parent: WebView
        var webViewNavigationSubscriber: AnyCancellable? = nil
        var valueSubscriber: AnyCancellable? = nil
        
        init(_ uiWebView: WebView) {
            self.parent = uiWebView
        }
        
        deinit {
            webViewNavigationSubscriber?.cancel()
            valueSubscriber?.cancel()
        }
        
        // MARK: - JSON
        private func serialize(message: [String: Any], pretty: Bool) -> String? {
            var result: String?
            do {
                let data = try JSONSerialization.data(withJSONObject: message, options: pretty ? .prettyPrinted : JSONSerialization.WritingOptions(rawValue: 0))
                result = String(data: data, encoding: .utf8)
            } catch let error {
                print("serialization error: \(error.localizedDescription)")
            }
            return result
        }
        
        private func dispatch(message: [String: Any]) -> String? {
            guard var messageJSON = serialize(message: message, pretty: false) else { return nil }
            
            messageJSON = messageJSON.replacingOccurrences(of: "\\", with: "\\\\")
            messageJSON = messageJSON.replacingOccurrences(of: "\"", with: "\\\"")
            messageJSON = messageJSON.replacingOccurrences(of: "\'", with: "\\\'")
            messageJSON = messageJSON.replacingOccurrences(of: "\n", with: "\\n")
            messageJSON = messageJSON.replacingOccurrences(of: "\r", with: "\\r")
            messageJSON = messageJSON.replacingOccurrences(of: "\u{000C}", with: "\\f")
            messageJSON = messageJSON.replacingOccurrences(of: "\u{2028}", with: "\\u2028")
            messageJSON = messageJSON.replacingOccurrences(of: "\u{2029}", with: "\\u2029")

            return "valueGotFromIOS('\(messageJSON)');"
        }
        
        func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
            /* An observer that observes 'viewModel.valuePublisher' to get value from TextField and
             pass that value to web app by calling JavaScript function */
            valueSubscriber = parent.viewModel.valuePublisher.receive(on: RunLoop.main).sink(receiveValue: { value in
                if let javascriptFunction = self.dispatch(message: value) {
                    webView.evaluateJavaScript(javascriptFunction) { (response, error) in
                        if let error = error {
                            print("Error calling javascript:valueGotFromIOS()")
                            print(error.localizedDescription)
                        } else {
                            print("Called javascript:valueGotFromIOS()")
                        }
                    }
                }
            })
            
            // Page loaded so no need to show loader anymore
            self.parent.viewModel.showLoader.send(false)
        }
        
        func webViewWebContentProcessDidTerminate(_ webView: WKWebView) {
            parent.viewModel.showLoader.send(false)
        }
        
        func webView(_ webView: WKWebView, didFail navigation: WKNavigation!, withError error: Error) {
            parent.viewModel.showLoader.send(false)
        }
        
        func webView(_ webView: WKWebView, didCommit navigation: WKNavigation!) {
            parent.viewModel.showLoader.send(true)
        }
        
        func webView(_ webView: WKWebView, didStartProvisionalNavigation navigation: WKNavigation!) {
            parent.viewModel.showLoader.send(true)
            self.webViewNavigationSubscriber = self.parent.viewModel.webViewNavigationPublisher.receive(on: RunLoop.main).sink(receiveValue: { navigation in
                switch navigation {
                    case .backward:
                        if webView.canGoBack {
                            webView.goBack()
                        }
                    case .forward:
                        if webView.canGoForward {
                            webView.goForward()
                        }
                }
            })
        }
        
        // This function is essential for intercepting every navigation in the webview
        func webView(_ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction, decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
            // Suppose you don't want your user to go a restricted site
            if let host = navigationAction.request.url?.host {
                if host == "restricted.com" {
                    // Navigation is cancelled
                    decisionHandler(.cancel)
                    return
                }
            }
            decisionHandler(.allow)
        }
    }
}

extension WebView.Coordinator: WKScriptMessageHandler {
    func userContentController(_ userContentController: WKUserContentController, didReceive message: WKScriptMessage) {
        // Make sure that your passed delegate is called
        if message.name == "iOSNative" {
            if let body = message.body as? [String: Any?] {
                print("JSON value received from web is: \(body)")
            } else if let body = message.body as? String {
                print("String value received from web is: \(body)")
            }
        }
    }
}
