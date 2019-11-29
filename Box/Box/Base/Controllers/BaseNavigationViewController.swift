//
//  BaseNavigationViewController.swift
//  Box
//
//  Created by Yan Hu on 2019/11/25.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit
import SnapKit

class BaseNavigationViewController: UINavigationController, UIGestureRecognizerDelegate {

    override func viewDidLoad() {
        super.viewDidLoad()

        // Do any additional setup after loading the view.
        navigationBar.isHidden = true
        interactivePopGestureRecognizer?.delegate = self
    }

    /*
    // MARK: - Navigation

    // In a storyboard-based application, you will often want to do a little preparation before navigation
    override func prepare(for segue: UIStoryboardSegue, sender: Any?) {
        // Get the new view controller using segue.destination.
        // Pass the selected object to the new view controller.
    }
    */

}
