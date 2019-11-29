//
//  BaseSelectView.swift
//  Box
//
//  Created by Yan Hu on 2019/11/28.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit

class BaseSelectView: UIView {
    lazy private var iconView: UIImageView = {
        let view = UIImageView.view(with: "arrow-down")
        return view
    }()
    
    lazy private var titleLabel: UILabel = {
        let label = UILabel.title()
        return label
    }()
    
    override init(frame: CGRect) {
        super.init(frame: frame)
        cornerRadius = Config.cornerRadiusSmall
        backgroundColor = .purple
        addSubview(iconView)
        addSubview(titleLabel)
        
        titleLabel.snp.makeConstraints { (make) in
            make.centerY.equalTo(self)
            make.left.equalTo(Config.itemGap)
        }
        
        iconView.snp.makeConstraints { (make) in
            make.centerY.equalTo(self)
            make.left.equalTo(titleLabel.snp.right).offset(10)
        }
        
        snp.makeConstraints { (make) in
            make.right.equalTo(iconView).offset(Config.itemGap)
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
