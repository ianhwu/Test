//
//  ViewController.swift
//  TestModuleFramework
//
//  Created by yan on 2020/7/11.
//  Copyright © 2020 yan. All rights reserved.
//

import UIKit
import ModuleFramework

class ViewController: UIViewController {

    override func viewDidLoad() {
        super.viewDidLoad()
        // Do any additional setup after loading the view.
        let a = SwiftTest()
        a.cRun()
        
        let b = SwiftTest1()
        b.cRun()
        
        subRun()
    }
}
