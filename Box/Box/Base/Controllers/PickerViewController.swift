//
//  PickerView.swift
//  Box
//
//  Created by Yan Hu on 2019/11/28.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit

class PickerViewController: UIViewController {
    lazy private var titleLabel: UILabel = {
        let label = UILabel.title()
        return label
    }()
    
    lazy private var closeBtn = UIImageView.view(with: "close").tap {
        [weak self] in
        self?.hide()
    }
    
    lazy private var saveBtn: UIButton = {
       let btn = UIButton.init(type: .custom)
        btn.backgroundColor = .pinkRed
        btn.cornerRadius = Config.cornerRadiusSmall
        btn.setTitle("Save", for: .normal)
        btn.titleLabel?.font = .font14
        btn.setTitleColor(.white, for: .normal)
        btn.addTarget(self, action: #selector(save), for: .touchUpInside)
        return btn
    }()
    
    lazy private var pickerView: UIPickerView = {
        let pickerView = UIPickerView()
        pickerView.delegate = self
        pickerView.dataSource = self
        pickerView.showsSelectionIndicator = false
        return pickerView
    }()
    private let contentView = UIView()
    
    private var items: [String]
    init(_ items: [String]) {
        self.items = items
        super.init(nibName: nil, bundle: nil)
    }
    
    var didSelected: ((String) -> ())?
    var index = 0
    @objc private func save() {
        hide()
        guard !items.isEmpty else { return }
        didSelected?(items[index])
    }
    var current: String?
    
    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        UIView.animate(withDuration: 0.3) {
            self.view.backgroundColor = .black50
            self.contentView.topConstraint?.deactivate()
            self.contentView.snp.makeConstraints { (make) in
                self.contentView.bottomConstraint = make.bottom.equalTo(self.view).constraint
            }
            self.view.layoutIfNeeded()
        }
    }
    
    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        pickerView.subviews[1].isHidden = true
        pickerView.subviews[2].isHidden = true
    }
    
    override func viewDidLoad() {
        super.viewDidLoad()
        
        contentView.backgroundColor = .modena
        
        view.addSubview(contentView)
        contentView.addSubview(titleLabel)
        contentView.addSubview(closeBtn)
        contentView.addSubview(saveBtn)
        contentView.addSubview(pickerView)
        
        saveBtn.snp.makeConstraints { (make) in
            make.bottom.equalTo(-10)
            make.height.equalTo(50)
            make.left.equalTo(Config.margin)
            make.right.equalTo(-Config.margin)
        }
        
        pickerView.snp.makeConstraints { (make) in
            make.left.right.equalTo(saveBtn)
            make.bottom.equalTo(saveBtn.snp.top).offset(-10)
        }
        
        let lineView = UIView.line()
        contentView.addSubview(lineView)
        lineView.snp.makeConstraints { (make) in
            make.left.right.equalTo(saveBtn)
            make.bottom.equalTo(pickerView.snp.top).offset(-10)
        }
        
        titleLabel.snp.makeConstraints { (make) in
            make.centerX.equalTo(contentView)
            make.height.equalTo(45)
            make.bottom.equalTo(lineView.snp.top)
        }
        
        closeBtn.snp.makeConstraints { (make) in
            make.centerY.equalTo(titleLabel)
            make.right.equalTo(saveBtn)
            make.width.height.equalTo(18)
        }
        
        contentView.snp.makeConstraints { (make) in
            make.left.right.equalTo(view)
            make.top.equalTo(self.titleLabel)
            contentView.topConstraint = make.top.equalTo(view.snp.bottom).constraint
        }
        view.backgroundColor = .clear
        
        if let current = current {
            for i in 0 ..< items.count {
                if items[i] == current {
                    pickerView.selectRow(i, inComponent: 0, animated: false)
                    break
                }
            }
        }
    }
    
    private func hide() {
        UIView.animate(withDuration: 0.3, animations: {
            self.view.backgroundColor = .clear
            self.contentView.bottomConstraint?.deactivate()
            self.contentView.snp.makeConstraints { (make) in
                make.top.equalTo(self.view.snp.bottom)
            }
            self.view.layoutIfNeeded()
        }) { _ in
            self.dismiss(animated: false, completion: nil)
        }
    }
    
    @discardableResult
    func title(_ text: String?) -> Self {
        titleLabel.text = text
        return self
    }
    
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }
}

extension PickerViewController: UIPickerViewDelegate, UIPickerViewDataSource {
    func pickerView(_ pickerView: UIPickerView, didSelectRow row: Int, inComponent component: Int) {
        index = row
    }
    
    func numberOfComponents(in pickerView: UIPickerView) -> Int {
        return 1
    }
    
    func pickerView(_ pickerView: UIPickerView, numberOfRowsInComponent component: Int) -> Int {
        return items.count
    }
    
    func pickerView(_ pickerView: UIPickerView, rowHeightForComponent component: Int) -> CGFloat {
        return 40
    }
    
    func pickerView(_ pickerView: UIPickerView, attributedTitleForRow row: Int, forComponent component: Int) -> NSAttributedString? {
        return NSAttributedString.init(string: items[row], attributes: [.foregroundColor : UIColor.white])
    }
}

extension UIViewController {
    func picker(_ items: [String], _ title: String?,_ current: String? = nil, _ didSelected: ((String) -> ())?) {
        let vc = PickerViewController(items).title(title)
        vc.current = current
        vc.didSelected = didSelected
        vc.modalPresentationStyle = .overCurrentContext
        present(vc, animated: false, completion: nil)
    }
}
