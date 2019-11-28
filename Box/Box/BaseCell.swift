//
//  BaseCell.swift
//  Box
//
//  Created by Yan Hu on 2019/11/28.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit

class BaseCell: UIView {
    lazy private var lineView: UIView = {
        let view = UIView()
        view.backgroundColor = .white10
        return view
    }()
    
    var left = 0 {
        didSet {
            lineView.leftConstraint?.update(offset: right)
        }
    }
    
    var right: CGFloat = 0 {
        didSet {
            lineView.rightConstraint?.update(offset: right)
        }
    }
    
    var cellHeight = Config.cellHeight {
        didSet {
            heightConstraint?.deactivate()
            snp.makeConstraints { (make) in
                heightConstraint = make.height.equalTo(cellHeight).constraint
            }
        }
    }
    var itemGap: CGFloat = Config.itemGap
    
    override init(frame: CGRect) {
        super.init(frame: frame)
        addSubview(lineView)
        lineView.snp.makeConstraints { (make) in
            lineView.leftConstraint = make.left.equalTo(left).constraint
            lineView.rightConstraint = make.right.equalTo(right).constraint
            make.bottom.equalTo(self)
            make.height.equalTo(Config.lineHeight)
        }
        
        snp.makeConstraints { (make) in
            heightConstraint = make.height.equalTo(cellHeight).constraint
        }
    }
    
    @discardableResult
    func lineIsHidden(_ hidden: Bool) -> Self {
        lineView.isHidden = hidden
        return self
    }
    
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }
    
}
