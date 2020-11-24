//
//  ViewController.swift
//  LeaksTest
//
//  Created by Yan Hu on 2019/12/23.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit

class ViewController: UIViewController {

    override func viewDidLoad() {
        super.viewDidLoad()
        // Do any additional setup after loading the view.

        let sub = UIView.init(frame: view.bounds)
        sub.backgroundColor = .red
        view.addSubview(sub)
    }
}

final class Deallocator {
    var closure: () -> Void
    init(_ closure: @escaping () -> Void) {
        self.closure = closure
    }

    deinit {
        closure()
    }
}

extension NSObject {
    static let weakTabel = NSHashTable<AnyObject>.weakObjects()
    private struct Keys {
        static var deallocator: Void?
    }
    
    var deallocator: Deallocator {
        if let dea = objc_getAssociatedObject(self, &Keys.deallocator) as? Deallocator {
            return dea
        }
        let dea = Deallocator.init {
            [weak self] in
            NSObject.weakTabel.remove(self)
        }
        return dea
    }
    
    static let classInit: Void = {
        swizzleMethodSelector(#selector(NSObject.initialize), withSelector: #selector(NSObject.run))
    }()
    
    @objc static func run() {
        
    }
}

public extension NSObject {
    class func swizzleMethodSelector(_ originalSelector: Selector, withSelector swizzledSelector: Selector){
        
        guard let originalMethod = class_getInstanceMethod(self, originalSelector),
            let swizzledMethod = class_getInstanceMethod(self, swizzledSelector) else {
                return
        }
        let didAddMethod = class_addMethod(self, originalSelector, method_getImplementation(swizzledMethod), method_getTypeEncoding(swizzledMethod))
        if didAddMethod {
            class_replaceMethod(self, swizzledSelector, method_getImplementation(originalMethod), method_getTypeEncoding(originalMethod))
        } else {
            method_exchangeImplementations(originalMethod, swizzledMethod)
        }
    }
}
