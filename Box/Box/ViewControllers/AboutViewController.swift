//
//  AboutViewController.swift
//  Box
//
//  Created by Yan Hu on 2019/11/27.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit

class AboutViewController: BaseViewController {

    override func viewDidLoad() {
        super.viewDidLoad()

        // Do any additional setup after loading the view.
        title = "About"
        
        let appIconView = UIImageView()
        appIconView.contentMode = .scaleAspectFit
        appIconView.image = UIImage.init(named: "app-icon")
        view.addSubview(appIconView)
        appIconView.snp.makeConstraints { (make) in
            make.centerX.equalTo(view)
            make.width.height.equalTo(66)
            make.top.equalTo(navBar.snp.bottom).offset(80)
        }
        
        let nameLabel = UILabel()
        nameLabel.text = "DiaBox"
        nameLabel.textColor = .white
        nameLabel.font = .font16
        view.addSubview(nameLabel)
        
        nameLabel.snp.makeConstraints { (make) in
            make.centerX.equalTo(view)
            make.top.equalTo(appIconView.snp.bottom).offset(24)
        }
        
        let versionNameLabel = UILabel.title()
        versionNameLabel.text = "Version"
        view.addSubview(versionNameLabel)
        
        versionNameLabel.snp.makeConstraints { (make) in
            make.left.equalTo(margin)
            make.top.equalTo(nameLabel.snp.bottom).offset(30)
        }
        
        let versionValueLabel = UILabel.title()
        versionValueLabel.text = UIApplication.buildVersion
        view.addSubview(versionValueLabel)
        
        versionValueLabel.snp.makeConstraints { (make) in
            make.right.equalTo(-margin)
            make.top.equalTo(versionNameLabel)
        }
        
        let lineView = UIView()
        lineView.backgroundColor = .white10
        view.addSubview(lineView)
        lineView.snp.makeConstraints { (make) in
            make.left.equalTo(versionNameLabel)
            make.right.equalTo(versionValueLabel)
            make.height.equalTo(1)
            make.top.equalTo(versionNameLabel.snp.bottom).offset(8)
        }
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
