//
//  UIViewExtension.swift
//  Box
//
//  Created by Yan Hu on 2019/11/26.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit
import SnapKit
import Closures

extension String {
    func width(fontSize: CGFloat) -> CGFloat {
        let font = UIFont.systemFont(ofSize: fontSize)
        let rect = NSString(string: self).boundingRect(with: CGSize(width: CGFloat(MAXFLOAT), height: fontSize + 2), options: .usesLineFragmentOrigin, attributes: [.font: font], context: nil)
        return ceil(rect.width)
    }
}

extension UIView {
    func show(msgs: [String], max: CGFloat = 0, selected: ((String) -> ())?) {
        guard let window = UIApplication.shared.keyWindow, let rect = superview?.convert(frame, to: window) else { return }
        let itemHeight: CGFloat = 30
        let gap: CGFloat = 10
        var maxHeight: CGFloat = CGFloat(msgs.count) * itemHeight
        if max != 0 { maxHeight = max }
        let view = UIView.init(frame: window.bounds)
        window.addSubview(view)
        
        view.addTapGesture { [weak view] (_) in
            view?.removeFromSuperview()
        }
        
        let scrollView = UIScrollView()
        view.addSubview(scrollView)
        
        let contentView = UIView()
        contentView.backgroundColor = .purple
        scrollView.addSubview(contentView)
        contentView.snp.makeConstraints { (make) in
            make.edges.width.equalTo(scrollView)
        }
        
        var top = contentView.snp.top
        var maxWidth: CGFloat = 10
        for msg in msgs {
            let label = UILabel()
            label.textColor = .white
            label.text = msg
            label.isUserInteractionEnabled = true
            label.addTapGesture { [weak view] (_) in
                view?.removeFromSuperview()
                selected?(msg)
            }
            contentView.addSubview(label)
            label.snp.makeConstraints { (make) in
                make.top.equalTo(top)
                make.height.equalTo(itemHeight)
                make.left.equalTo(gap)
                make.right.equalTo(-gap)
            }
            top = label.snp.bottom
            let width = msg.width(fontSize: label.font.pointSize) + gap * 2
            if width > maxWidth {
                maxWidth = width
            }
        }
        scrollView.frame = CGRect.init(x: rect.origin.x, y: rect.origin.y, width: maxWidth, height: maxHeight)
        
        contentView.snp.makeConstraints { (make) in
            make.bottom.equalTo(top)
        }
        
        UIView.animate(withDuration: 0.3) {
            scrollView.frame = CGRect.init(x: rect.origin.x, y: rect.origin.y + rect.height, width: maxWidth, height: maxHeight)
        }
    }
}
