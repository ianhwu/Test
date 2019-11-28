//
//  UIImageViewExtension.swift
//  Box
//
//  Created by Yan Hu on 2019/11/27.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit
import Closures

extension UIImageView {
    static func view(with name: String) -> UIImageView {
        let imageView = UIImageView()
        imageView.image = UIImage.init(named: name)
        imageView.contentMode = .scaleAspectFit
        return imageView
    }
    
    static func scaleAspectFitImageView() -> UIImageView {
        let imageView = UIImageView()
        imageView.contentMode = .scaleAspectFit
        return imageView
    }
    
    @discardableResult
    func tap(_ action: (() -> ())?) -> Self {
        isUserInteractionEnabled = true
        addTapGesture { _ in
            action?()
        }
        return self
    }
}
