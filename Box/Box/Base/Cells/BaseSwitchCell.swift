//
//  BaseSwitchCell.swift
//  Box
//
//  Created by Yan Hu on 2019/11/28.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit

class BaseSwitchCell: BaseCell {
    lazy private var titleLabel: UILabel = {
        let label = UILabel.title()
        return label
    }()
    
    lazy private var descriptionLabel: UILabel = {
        let label = UILabel.white8()
        return label
    }()
    
    lazy private var switchView: UIButton = {
        let btn = UIButton.init(type: .custom)
        btn.setImage(UIImage.init(named: "switch-on"), for: .normal)
        btn.setImage(UIImage.init(named: "switch-off"), for: .selected)
        btn.addTarget(self, action: #selector(btnAction), for: .touchUpInside)
        return btn
    }()
    
    private var action: ((Bool) -> ())?
    
    @objc private func btnAction() {
        switchView.isSelected = !switchView.isSelected
        action?(switchView.isSelected)
    }
    
    override init(frame: CGRect) {
        super.init(frame: frame)
        
        addSubview(titleLabel)
        addSubview(descriptionLabel)
        addSubview(switchView)
        
        titleLabel.snp.makeConstraints { (make) in
            make.left.equalTo(self)
            make.top.equalTo(cellHeight * 0.1)
            make.height.equalTo(self).multipliedBy(0.5)
        }
        
        descriptionLabel.snp.makeConstraints { (make) in
            make.left.equalTo(self)
            make.bottom.equalTo(-cellHeight * 0.1)
            make.height.equalTo(self).multipliedBy(0.3)
        }
        
        switchView.snp.makeConstraints { (make) in
            make.centerY.equalTo(self)
            make.right.equalTo(-itemGap)
        }
    }
    
    @discardableResult
    func title(_ text: String?) -> Self {
        titleLabel.text = text
        return self
    }
    
    @discardableResult
    func description(_ text: String?) -> Self {
        descriptionLabel.text = text
        return self
    }
    
    @discardableResult
    func setOn(_ on: Bool) -> Self {
        switchView.isSelected = on
        return self
    }
    
    @discardableResult
    func action(_ action: ((Bool) -> ())?) -> Self {
        self.action = action
        return self
    }
    
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }
}
