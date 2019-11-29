//
//  BasePickerCell.swift
//  Box
//
//  Created by Yan Hu on 2019/11/29.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit

class BasePickerCell: BaseCell {
    lazy private var iconView: UIImageView = {
        let view = UIImageView.scaleAspectFitImageView()
        view.isHidden = true
        return view
    }()
    
    lazy private var titleLabel: UILabel = {
        let label = UILabel.title()
        return label
    }()
    
    lazy private var valueLabel: UILabel = {
        let label = UILabel.value()
        return label
    }()
    
    lazy private var arrowView: UIImageView = {
        let view = UIImageView.view(with: "arrow-right")
        return view
    }()
    
    private var tap: (() -> ())?
    func action(_ tap: (() -> ())?) {
        self.tap = tap
    }
    
    override init(frame: CGRect) {
        super.init(frame: frame)
        
        addTapGesture { [weak self] _ in
            self?.tap?()
        }
        
        addSubview(iconView)
        addSubview(titleLabel)
        addSubview(arrowView)
        addSubview(valueLabel)
        
        iconView.snp.makeConstraints { (make) in
            make.left.centerY.equalTo(self)
        }
        
        titleLabel.snp.makeConstraints { (make) in
            titleLabel.leftConstraint = make.left.equalTo(self).constraint
            make.centerY.equalTo(self)
        }
        
        arrowView.snp.makeConstraints { (make) in
            make.right.equalTo(-itemGap)
            make.centerY.equalTo(self)
        }
        
        valueLabel.snp.makeConstraints { (make) in
            make.centerY.equalTo(self)
            make.right.equalTo(arrowView.snp.left).offset(-itemGap)
        }
    }
    
    @discardableResult
    func icon(_ text: String?) -> Self {
        titleLabel.leftConstraint?.deactivate()
        if let text = text, let image = UIImage.init(named: text) {
            iconView.image = image
            iconView.isHidden = false
            titleLabel.snp.makeConstraints { (make) in
                titleLabel.leftConstraint = make.left.equalTo(iconView.snp.right).offset(itemGap).constraint
            }
        } else {
            iconView.isHidden = true
            titleLabel.snp.makeConstraints { (make) in
                titleLabel.leftConstraint = make.left.equalTo(self).constraint
            }
        }
        return self
    }
    
    @discardableResult
    func title(_ text: String?) -> Self {
        titleLabel.text = text
        return self
    }
    
    @discardableResult
    func value(_ text: String?) -> Self {
        valueLabel.text = text
        return self
    }
    
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }
}
