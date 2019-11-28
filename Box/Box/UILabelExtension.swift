//
//  UILabelExtension.swift
//  Box
//
//  Created by Yan Hu on 2019/11/27.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit

extension UILabel {
    static func white() -> UILabel {
        let label = UILabel()
        label.textColor = .white
        return label
    }
    
    static func white16() ->  UILabel {
        let label = white()
        label.font = .font16
        return label
    }
    
    static func white14() ->  UILabel {
        let label = white()
        label.font = .font14
        return label
    }
    
    static func white12() ->  UILabel {
        let label = white()
        label.font = .font12
        return label
    }
    
    static func white8() ->  UILabel {
        let label = white()
        label.font = .font8
        return label
    }
    
    static func white10() ->  UILabel {
        let label = white()
        label.font = .font10
        return label
    }
    
    static func title() ->  UILabel {
        return white14()
    }
}
