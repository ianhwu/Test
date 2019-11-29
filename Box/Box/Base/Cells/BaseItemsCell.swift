//
//  BaseItemsCell.swift
//  Box
//
//  Created by Yan Hu on 2019/11/28.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit
import Closures

class BaseItemsCell: BaseCell {
    private var titles: [String]
    private var didClick: ((String) -> ())?
    init(_ titles: [String], _ gap: CGFloat = 30) {
        self.titles = titles
        super.init(frame: .zero)
        guard !titles.isEmpty else { return }

        let itemWidth = 80
        let contentView = UIView()
        addSubview(contentView)
        var left = contentView.snp.left
        var oset: CGFloat = 0
        for title in titles {
            let button = UIButton.init(type: .custom)
            button.cornerRadius = Config.cornerRadiusSmall
            button.setTitle(title, for: .normal)
            button.setTitleColor(.white, for: .normal)
            button.titleLabel?.font = .font12
            button.backgroundColor = .purple
            contentView.addSubview(button)
            
            button.addTapGesture { [weak self] _ in
                self?.didClick?(title)
            }

            button.snp.makeConstraints { (make) in
                make.top.bottom.equalTo(contentView)
                make.left.equalTo(left).offset(oset)
                make.width.equalTo(itemWidth)
            }
            left = button.snp.right
            oset = gap
        }

        contentView.snp.makeConstraints { (make) in
            make.center.equalTo(self)
            make.height.equalTo(self).offset(-itemGap * 2)
            make.right.equalTo(left)
        }
    }
    
    func didSelected(_ action: ((String) -> ())?) {
        didClick = action
    }
    
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }
}
