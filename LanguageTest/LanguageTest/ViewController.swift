//
//  ViewController.swift
//  LanguageTest
//
//  Created by Yan Hu on 2020/1/19.
//  Copyright © 2020 cn.com.yan. All rights reserved.
//

import UIKit
import Rswift

class ViewController: UIViewController {
    
    let label = UILabel()
    override func viewDidLoad() {
        super.viewDidLoad()
        // Do any additional setup after loading the view.
        
        if let preferredLang = Bundle.main.preferredLocalizations.first {
            if preferredLang == "zh" {
                UserDefaultsUnit.language = "zh"
            } else {
                UserDefaultsUnit.language = "en"
            }
        }
        
        
        view.addSubview(label)
        label.textAlignment = .center
        label.frame = view.bounds
        
        UserDefaults.standard.addObserver(self, forKeyPath: "language", options: .new, context: nil)
        resetText()
    }
    
    private func resetText() {
        label.text = R.string.localizable.hahaha(preferredLanguages: [UserDefaultsUnit.language])
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        if UserDefaultsUnit.language == "zh" {
            UserDefaultsUnit.language = "en"
        } else {
            UserDefaultsUnit.language = "zh"
        }
        UserDefaults.standard.set([UserDefaultsUnit.language], forKey: "AppleLanguages")
        UserDefaults.standard.synchronize()
    }
    
    override func observeValue(forKeyPath keyPath: String?, of object: Any?, change: [NSKeyValueChangeKey : Any]?, context: UnsafeMutableRawPointer?) {
        if let keyPath = keyPath, keyPath == "language" {
            resetText()
        }
    }
    
    deinit {
        UserDefaults.standard.removeObserver(self, forKeyPath: "language")
    }
}

@propertyWrapper
struct UserDefaultWrapper<T> {
    var key: String
    var defaultT: T!
    var wrappedValue: T! {
        get { (UserDefaults.standard.object(forKey: key) as? T) ?? defaultT }
        nonmutating set {
            if newValue == nil {
                UserDefaults.standard.removeObject(forKey: key)
            } else {
                UserDefaults.standard.set(newValue, forKey: key)
            }
        }
    }
    
    init(_ key: String, _ defaultT: T! = nil) {
        self.key = key
        self.defaultT = defaultT
    }
}
struct UserDefaultsUnit {
    @UserDefaultWrapper("language", "zh")
    static var language: String!
}
