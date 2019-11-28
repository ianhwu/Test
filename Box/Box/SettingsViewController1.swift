//
//  SettingsViewController1.swift
//  Box
//
//  Created by Yan Hu on 2019/11/28.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit

class SettingsViewController1: BaseScrollViewController {
    lazy private var languageView: BaseSelectCell = {
        let view = BaseSelectCell.init(["中文", "English"]).title("Language").itemTitle("English")
        view.didSelect = {
            text in
        }
        return view
    }()
    
    lazy private var dataView: BaseSelectCell = {
        let view = BaseSelectCell.init(["Master", "Follower"]).title("Data Collection").itemTitle("Master")
        view.didSelect = {
            text in
        }
        return view
    }()
    
    lazy private var itemsView: BaseItemsCell = {
        let view = BaseItemsCell(["Abbott", "Dexcom"])
        view.didSelected { [weak self] (text) in
            if text == "Abbott" {
                
            } else {
                
            }
        }
        return view
    }()
    
    lazy private var formatView: BaseSelectCell = {
        let view = BaseSelectCell.init(["12H", "24H"]).title("Time Format").itemTitle("12H")
        view.didSelect = {
            text in
        }
        return view
    }()
    
    lazy private var unitsView: BaseSelectCell = {
        let view = BaseSelectCell.init(["mmol/L", "mg/dl"]).title("Units").itemTitle("mmol/L")
        view.didSelect = {
            text in
        }
        return view
    }()
    
    lazy private var eA1CView: BaseSelectCell = {
        let view = BaseSelectCell.init(["eA1C", "IFCC"]).title("Data Collection").itemTitle("eA1C")
        view.didSelect = {
            text in
        }
        return view
    }()
    
    override func viewDidLoad() {
        super.viewDidLoad()

        // Do any additional setup after loading the view.
        title = "Setting"
        
        setupView()
    }
    
    private func setupView() {
        contentView.addSubview(languageView)
        contentView.addSubview(dataView)
        contentView.addSubview(itemsView)
        contentView.addSubview(formatView)
        contentView.addSubview(unitsView)
        contentView.addSubview(eA1CView)
//        contentView.addSubview(dataView)
        
        // select
        languageView.snp.makeConstraints { (make) in
            make.top.equalTo(contentView)
            make.left.equalTo(margin)
            make.right.equalTo(-margin)
        }
        
        dataView.snp.makeConstraints { (make) in
            make.left.right.equalTo(languageView)
            make.top.equalTo(languageView.snp.bottom)
        }
        
        itemsView.snp.makeConstraints { (make) in
            make.left.right.equalTo(languageView)
            make.top.equalTo(dataView.snp.bottom)
        }
        
        formatView.snp.makeConstraints { (make) in
            make.left.right.equalTo(languageView)
            make.top.equalTo(itemsView.snp.bottom)
        }
        
        unitsView.snp.makeConstraints { (make) in
            make.left.right.equalTo(languageView)
            make.top.equalTo(formatView.snp.bottom)
        }
        
        eA1CView.snp.makeConstraints { (make) in
            make.left.right.equalTo(languageView)
            make.top.equalTo(unitsView.snp.bottom)
        }
        
        // picker
        
        contentView.snp.makeConstraints { (make) in
            make.bottom.equalTo(eA1CView)
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
