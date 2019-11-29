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
        let view = BaseSelectCell.init(["English", "中文"]).title("Language").itemTitle("English")
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
                let vc = ScanViewController()
                self?.navigationController?.pushViewController(vc)
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
            [weak self] text in
            self?.updateValues()
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
    
    let extremeHighView = BasePickerCell().title("Extreme High").icon("water-pink")
    let highView = BasePickerCell().title("High").icon("water-orange")
    let targetView = BasePickerCell().title("Target").icon("water-green")
    let lowView = BasePickerCell().title("Extreme High").icon("water-orange")
    let urgentLowView = BasePickerCell().title("Urgent Low").icon("water-pink")
    let graphHeightView = BasePickerCell().title("graphHeight").lineIsHidden(true)
    
    override func viewDidLoad() {
        super.viewDidLoad()

        // Do any additional setup after loading the view.
        title = "Setting"
        
        setupView()
        setActions()
        updateValues()
    }
    
    private func updateValues() {
        extremeHighView.value("100")
        highView.value("100")
        targetView.value("100")
        lowView.value("100")
        urgentLowView.value("100")
        graphHeightView.value("100")
    }
    
    private func setActions() {
        extremeHighView.action {
            [weak self] in
            self?.picker("Extreme High" ,72, 200, 1, "100", didSelected: { (text) in
                print(text)
            })
        }
        
        highView.action {
            [weak self] in
            self?.picker("High" ,72, 200, 1, didSelected: { (text) in
                print(text)
            })
        }
        
        targetView.action {
            [weak self] in
            self?.picker("Target" ,72, 200, 1, didSelected: { (text) in
                print(text)
            })
        }
        
        lowView.action {
            [weak self] in
            self?.picker("Low" ,72, 200, 1, didSelected: { (text) in
                print(text)
            })
        }
        
        urgentLowView.action {
            [weak self] in
            self?.picker("Urgent Low" ,72, 200, 1, didSelected: { (text) in
                print(text)
            })
        }
        
        graphHeightView.action {
            [weak self] in
            self?.picker("Graph Height" ,72, 200, 10000, didSelected: { (text) in
                print(text)
            })
        }
    }
    
    /// unit 大于 0.0001
    private func picker(_ title: String?, _ start: Float, _ end: Float, _ unit: Float, _ current: String? = nil, _ isInt: Bool = true, didSelected: ((String) -> ())?) {
        func string(_ value: Float) -> String {
            if isInt {
                return value.int.description
            } else {
                return String.init(format: "%.1f", value)
            }
        }
        
        var items = [String]()
        let count = Int((end - start) / unit)
        items.append(string(start))
        if count >  0 {
            for i in 0 ..< count - 1 {
                items.append(string(start + (i + 1).float * unit))
            }
        }
        items.append(string(end))
        
        picker(items, title, current) {
            [weak self] (text) in
            didSelected?(text)
            self?.updateValues()
        }
    }
    
    private func setupView() {
        contentView.addSubview(languageView)
        contentView.addSubview(dataView)
        contentView.addSubview(itemsView)
        contentView.addSubview(formatView)
        contentView.addSubview(unitsView)
        contentView.addSubview(eA1CView)
        
        contentView.addSubview(extremeHighView)
        contentView.addSubview(highView)
        contentView.addSubview(targetView)
        contentView.addSubview(lowView)
        contentView.addSubview(urgentLowView)
        contentView.addSubview(graphHeightView)
        
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
        extremeHighView.snp.makeConstraints { (make) in
            make.left.equalTo(margin)
            make.top.equalTo(eA1CView.snp.bottom)
            make.right.equalTo(contentView)
        }
        
        highView.snp.makeConstraints { (make) in
            make.left.right.equalTo(extremeHighView)
            make.top.equalTo(extremeHighView.snp.bottom)
        }
        
        targetView.snp.makeConstraints { (make) in
            make.left.right.equalTo(extremeHighView)
            make.top.equalTo(highView.snp.bottom)
        }
        
        lowView.snp.makeConstraints { (make) in
            make.left.right.equalTo(extremeHighView)
            make.top.equalTo(targetView.snp.bottom)
        }
        
        urgentLowView.snp.makeConstraints { (make) in
            make.left.right.equalTo(extremeHighView)
            make.top.equalTo(lowView.snp.bottom)
        }
        
        graphHeightView.snp.makeConstraints { (make) in
            make.left.right.equalTo(extremeHighView)
            make.top.equalTo(urgentLowView.snp.bottom)
        }
        
        contentView.snp.makeConstraints { (make) in
            make.bottom.equalTo(graphHeightView)
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
