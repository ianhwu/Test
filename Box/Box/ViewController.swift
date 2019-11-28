//
//  ViewController.swift
//  Box
//
//  Created by Yan Hu on 2019/11/25.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit

class ViewController: BaseViewController {

    override func viewDidLoad() {
        super.viewDidLoad()
        // Do any additional setup after loading the view.
        view.backgroundColor = .orange
        title = "Title"
        
        let button = UIButton()
        button.addTapGesture { (_) in
            self.picker(["1.1", "1.2"]) { _ in
                
            }
        }
        button.frame = CGRect.init(x: 100, y: 50, width: 30, height: 80)
        button.backgroundColor = .black
        view.addSubview(button)
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        let vc = SettingViewController()
        navigationController?.pushViewController(vc, animated: true)
    }
}

