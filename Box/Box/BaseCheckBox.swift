//
//  BaseCheckBox.swift
//  Box
//
//  Created by Yan Hu on 2019/11/28.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit

class BaseCheckBox: BaseCell {
    lazy private var button: UIButton = {
        let btn = UIButton.init(type: .custom)
        btn.setImage(UIImage.init(named: "main_line_normal"), for: .normal)
        btn.setImage(UIImage.init(named: "main_line_select"), for: .selected)
        btn.addTarget(self, action: #selector(btnAction), for: .touchUpInside)
        return btn
    }()
    
    lazy private var titleLabel: UILabel = {
        let label = UILabel.title()
        return label
    }()
    
    var isSelected: Bool {
        set { button.isSelected = newValue }
        get { button.isSelected }
    }
    var selectAction: ((_ selected: Bool) -> ())?
    override init(frame: CGRect) {
        super.init(frame: frame)
        let contentView = UIView()
        
        addSubview(contentView)
        contentView.addSubview(button)
        contentView.addSubview(titleLabel)
        
        button.snp.makeConstraints { (make) in
            make.width.height.equalTo(16)
            make.left.equalTo(contentView)
            make.centerY.equalTo(contentView)
        }
        
        
        titleLabel.snp.makeConstraints { (make) in
            make.left.equalTo(button.snp.right).offset(3)
            make.centerY.equalTo(contentView)
        }
        
        contentView.snp.makeConstraints { (make) in
            make.center.height.equalTo(self)
            make.right.equalTo(titleLabel)
        }
    }
    
    @objc private func btnAction() {
        button.isSelected = !button.isSelected
        selectAction?(button.isSelected)
    }
    
    @discardableResult
    func title(_ title: String?) -> Self {
        titleLabel.text = title
        return self
    }
    
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }
}
