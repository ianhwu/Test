//
//  IntegrationViewController.swift
//  Box
//
//  Created by Yan Hu on 2019/11/28.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit

class IntegrationViewController: BaseScrollViewController {
    let dexcomLabel = BaseTitleCell().title("Dexcom Share Credentials")
    let nameCell = BaseTextFieldCell().title("Username")
    let pwCell = BaseTextFieldCell().title("Password").lineIsHidden(true)
    lazy private var usCheckBox: BaseCheckBox = {
        let usCheckBox = BaseCheckBox().title("US Account")
        usCheckBox.selectAction = {
            [weak self] selected in
            
        }
        return usCheckBox
    }()
    
    lazy private var loginCell: BaseItemsCell = {
        let view = BaseItemsCell(["Log in"])
        view.didSelected { _ in
            
        }
        return view
    }()
    
    let nightscoutLabel = BaseTitleCell().title("Nightscout Share Credentials")
    let urlCell = BaseTextFieldCell().title("Url")
    let keyCell = BaseTextFieldCell().title("Key").lineIsHidden(true)
    
    lazy private var saveCell: BaseItemsCell = {
        let view = BaseItemsCell(["Save", "Connection"])
        view.didSelected { _ in
            
        }
        return view
    }()
    
    let serverCell = BaseSwitchCell().title("Internal HTTP Server").description("Android APS,Pebble,Garmin,Nightguard...Integratetion!")
        .action { (_) in
        
    }
    
    override func viewDidLoad() {
        super.viewDidLoad()

        // Do any additional setup after loading the view.
        title = "Integration"
        
        contentView.addSubview(dexcomLabel)
        contentView.addSubview(nameCell)
        contentView.addSubview(pwCell)
        contentView.addSubview(usCheckBox)
        contentView.addSubview(loginCell)
        
        contentView.addSubview(nightscoutLabel)
        contentView.addSubview(urlCell)
        contentView.addSubview(keyCell)
        contentView.addSubview(saveCell)
        contentView.addSubview(serverCell)
        
        // Dexcom
        dexcomLabel.snp.makeConstraints { (make) in
            make.top.right.equalTo(contentView)
            make.left.equalTo(margin)
        }
        
        nameCell.snp.makeConstraints { (make) in
            make.left.right.equalTo(dexcomLabel)
            make.top.equalTo(dexcomLabel.snp.bottom)
        }
        
        pwCell.snp.makeConstraints { (make) in
            make.left.right.equalTo(dexcomLabel)
            make.top.equalTo(nameCell.snp.bottom)
        }
        
        usCheckBox.snp.makeConstraints { (make) in
            make.left.right.equalTo(dexcomLabel)
            make.top.equalTo(pwCell.snp.bottom)
        }
        
        loginCell.snp.makeConstraints { (make) in
            make.left.right.equalTo(dexcomLabel)
            make.top.equalTo(usCheckBox.snp.bottom)
        }
        
        // nightscout
        nightscoutLabel.snp.makeConstraints { (make) in
            make.left.right.equalTo(dexcomLabel)
            make.top.equalTo(loginCell.snp.bottom)
        }
        
        urlCell.snp.makeConstraints { (make) in
            make.left.right.equalTo(dexcomLabel)
            make.top.equalTo(nightscoutLabel.snp.bottom)
        }
        
        keyCell.snp.makeConstraints { (make) in
            make.left.right.equalTo(dexcomLabel)
            make.top.equalTo(urlCell.snp.bottom)
        }
        
        saveCell.snp.makeConstraints { (make) in
            make.left.right.equalTo(dexcomLabel)
            make.top.equalTo(keyCell.snp.bottom)
        }
        
        serverCell.snp.makeConstraints { (make) in
            make.right.equalTo(contentView)
            make.left.equalTo(margin / 2)
            make.top.equalTo(saveCell.snp.bottom)
        }
        
        let descripLabel = UILabel.title()
        descripLabel.text = "Work as CGM data source for APS"
        descripLabel.textAlignment = .center
        
        contentView.addSubview(descripLabel)
        descripLabel.snp.makeConstraints { (make) in
            make.left.right.equalTo(dexcomLabel)
            make.top.equalTo(serverCell.snp.bottom).offset(margin)
        }
        
        // content view
        contentView.snp.makeConstraints { (make) in
            make.bottom.equalTo(descripLabel).offset(itemGap)
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

