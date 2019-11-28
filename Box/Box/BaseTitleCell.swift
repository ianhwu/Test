//
//  BaseTitleCell.swift
//  Box
//
//  Created by Yan Hu on 2019/11/28.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit

class BaseTitleCell: BaseCell {
    lazy private var titleLabel: UILabel = {
        let label = UILabel.title()
        label.textAlignment = .center
        return label
    }()
    
    override init(frame: CGRect) {
        super.init(frame: frame)
        addSubview(titleLabel)
        titleLabel.snp.makeConstraints { (make) in
            make.center.equalTo(self)
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
