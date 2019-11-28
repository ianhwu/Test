//
//  BaseSelectCell.swift
//  Box
//
//  Created by Yan Hu on 2019/11/28.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit

class BaseSelectCell: BaseCell {
    lazy private var titleLabel: UILabel = {
        let label = UILabel.title()
        return label
    }()
    
    lazy private var selectView: BaseSelectView = {
        let view = BaseSelectView()
        view.addTapGesture { [weak self] _ in
            self?.select()
        }
        return view
    }()
    
    var didSelect: ((String) -> ())?
    func select() {
        selectView.show(msgs: items) { [weak self] text in
            self?.selectView.title(text)
            self?.didSelect?(text)
        }
    }
    
    var items: [String]
    init(_ items: [String]) {
        self.items = items
        super.init(frame: .zero)
        addSubview(titleLabel)
        addSubview(selectView)
        
        titleLabel.snp.makeConstraints { (make) in
            make.left.equalTo(self)
            make.width.equalTo(100)
            make.centerY.equalTo(self)
        }
        
        selectView.snp.makeConstraints { (make) in
            make.right.equalTo(self)
            make.top.equalTo(itemGap)
            make.bottom.equalTo(-itemGap)
        }
    }
    
    @discardableResult
    func title(_ text: String?) -> Self {
        titleLabel.text = text
        return self
    }
    
    @discardableResult
    func itemTitle(_ text: String?) -> Self {
        selectView.title(text)
        return self
    }
    
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }
}
