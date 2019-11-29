//
//  UIButtonExtension.swift
//  Box
//
//  Created by Yan Hu on 2019/11/29.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit

extension UIButton {
    static func pink(_ title: String? = nil) -> UIButton {
        let btn = UIButton(type: .custom)
        btn.cornerRadius = Config.cornerRadiusSmall
        btn.backgroundColor = .pinkRed
        btn.setTitleColor(.white, for: .normal)
        btn.setTitle(title, for: .normal)
        btn.snp.makeConstraints { (make) in
            make.height.equalTo(50)
        }
        return btn
    }
    
    static func select(_ image: UIImage?, _ selected: UIImage? = nil) -> UIButton {
        let btn = UIButton(type: .custom)
        btn.setImage(image, for: .normal)
        btn.setImage(selected, for: .selected)
        return btn
    }
    
    func pink(_ title: String? = nil) {
        backgroundColor = .pinkRed
        setTitle(title, for: .normal)
        setTitleColor(.white, for: .normal)
    }
    
    func unSelected(_ title: String? = nil) {
        backgroundColor = .modena
        setTitle(title, for: .normal)
        setTitleColor(.white50, for: .normal)
    }
}
