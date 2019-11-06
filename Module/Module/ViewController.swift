//
//  ViewController.swift
//  Module
//
//  Created by Yan Hu on 2019/8/5.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit
import class OtherFile.Test.TestSubSub

class ViewController: UIViewController {

    override func viewDidLoad() {
        super.viewDidLoad()
        // Do any additional setup after loading the view.

//        let t = Test()
//        t.runn()
//
//        let t2 = Test2()
//        t2.runn2()
//
//        let p = file()
//        printFile(p)
//
//
//        run()
//        subRun()
//        subSubRun()
//        file3Run()
        
        let tSub = TestSubSub()
        tSub.testSubSubRun()
    }


}

