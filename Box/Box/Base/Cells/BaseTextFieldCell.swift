//
//  BaseTextFieldCell.swift
//  Box
//
//  Created by Yan Hu on 2019/11/28.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit

class BaseTextFieldCell: BaseCell {
    lazy private var titleLabel: UILabel = {
        let label = UILabel.title()
        return label
    }()
    
    lazy private var textField: UITextField = {
        let textField = UITextField()
        textField.font = .font12
        textField.backgroundColor = .purple
        textField.cornerRadius = Config.cornerRadiusSmall
        textField.clipsToBounds = true
        return textField
    }()
    
    var text: String? {
        return textField.text
    }
    
    override init(frame: CGRect) {
        super.init(frame: frame)
        
        addSubview(titleLabel)
        addSubview(textField)
        
        titleLabel.snp.makeConstraints { (make) in
            make.left.equalTo(self)
            make.width.equalTo(100)
            make.centerY.equalTo(self)
        }
        
        textField.snp.makeConstraints { (make) in
            make.top.equalTo(itemGap)
            make.right.bottom.equalTo(-itemGap)
            make.width.equalTo(self).offset(-100)
        }
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
