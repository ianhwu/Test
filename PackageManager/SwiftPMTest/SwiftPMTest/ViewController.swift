//
//  ViewController.swift
//  SwiftPMTest
//
//  Created by Yan Hu on 2019/10/22.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit
import HYImageBrowser
import Kingfisher
import RxSwift
import RxCocoa
import SPM_OCTest

class ViewController: UIViewController {

    override func viewDidLoad() {
        super.viewDidLoad()
        // Do any additional setup after loading the view.
        
        let image1 = UIImageView()
        view.addSubview(image1)
        view.contentMode = .scaleAspectFit
        image1.frame = .init(x: 100, y: 100, width: 100, height: 100)
        image1.kf.setImage(with: URL.init(string: "https://ss0.bdstatic.com/94oJfD_bAAcT8t7mm9GUKT-xh_/timg?image&quality=100&size=b4000_4000&sec=1571798387&di=396f475753003a0c281249a7f83a1946&src=http://b-ssl.duitang.com/uploads/item/201603/07/20160307231006_PL4ia.jpeg")!)
        
        let image2 = UIImageView()
        view.addSubview(image2)
        view.contentMode = .scaleAspectFit
        image2.frame = .init(x: 100, y: 300, width: 100, height: 100)
        image2.kf.setImage(with: URL.init(string: "https://ss0.bdstatic.com/94oJfD_bAAcT8t7mm9GUKT-xh_/timg?image&quality=100&size=b4000_4000&sec=1571798387&di=396f475753003a0c281249a7f83a1946&src=http://b-ssl.duitang.com/uploads/item/201603/07/20160307231006_PL4ia.jpeg")!)
        view.isAutoShowed = true
        
        let t = Test()
        t.testRun()
        
        let t2 = Test2()
        t2.testRun()
    }
}

