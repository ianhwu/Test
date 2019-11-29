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
            let label = UILabel.title()
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


extension UIView {
    private struct AssociatedKey {
        static var centerYConstraintKey: Void?
        static var centerXConstraintKey: Void?
        static var widthConstraintKey: Void?
        static var heightConstraintKey: Void?
        static var bottomConstraintKey: Void?
        static var topConstraintKey: Void?
        static var leftConstraintKey: Void?
        static var rightConstraintKey: Void?
        static var blankMaskKey: Void?
    }
    
    public var heightConstraint: Constraint?  {
        set { objc_setAssociatedObject(self, &AssociatedKey.heightConstraintKey, newValue, .OBJC_ASSOCIATION_RETAIN_NONATOMIC) }
        get { return objc_getAssociatedObject(self, &AssociatedKey.heightConstraintKey) as? Constraint }
    }
    
    public var widthConstraint: Constraint?  {
        set { objc_setAssociatedObject(self, &AssociatedKey.widthConstraintKey, newValue, .OBJC_ASSOCIATION_RETAIN_NONATOMIC) }
        get { return objc_getAssociatedObject(self, &AssociatedKey.widthConstraintKey) as? Constraint }
    }
    
    public var topConstraint: Constraint?  {
        set { objc_setAssociatedObject(self, &AssociatedKey.topConstraintKey, newValue, .OBJC_ASSOCIATION_RETAIN_NONATOMIC) }
        get { return objc_getAssociatedObject(self, &AssociatedKey.topConstraintKey) as? Constraint }
    }
    
    public var centerXConstraint: Constraint?  {
        set { objc_setAssociatedObject(self, &AssociatedKey.centerXConstraintKey, newValue, .OBJC_ASSOCIATION_RETAIN_NONATOMIC) }
        get { return objc_getAssociatedObject(self, &AssociatedKey.centerXConstraintKey) as? Constraint }
    }
    
    public var centerYConstraint: Constraint?  {
        set { objc_setAssociatedObject(self, &AssociatedKey.centerYConstraintKey, newValue, .OBJC_ASSOCIATION_RETAIN_NONATOMIC) }
        get { return objc_getAssociatedObject(self, &AssociatedKey.centerYConstraintKey) as? Constraint }
    }
    
    public var leftConstraint: Constraint?  {
        set { objc_setAssociatedObject(self, &AssociatedKey.leftConstraintKey, newValue, .OBJC_ASSOCIATION_RETAIN_NONATOMIC) }
        get { return objc_getAssociatedObject(self, &AssociatedKey.leftConstraintKey) as? Constraint }
    }
    
    public var bottomConstraint: Constraint?  {
        set { objc_setAssociatedObject(self, &AssociatedKey.bottomConstraintKey, newValue, .OBJC_ASSOCIATION_RETAIN_NONATOMIC) }
        get { return objc_getAssociatedObject(self, &AssociatedKey.bottomConstraintKey) as? Constraint }
    }
    
    public var rightConstraint: Constraint?  {
        set { objc_setAssociatedObject(self, &AssociatedKey.rightConstraintKey, newValue, .OBJC_ASSOCIATION_RETAIN_NONATOMIC) }
        get { return objc_getAssociatedObject(self, &AssociatedKey.rightConstraintKey) as? Constraint }
    }
}

extension UIView {
    static func line() -> UIView {
        let view = UIView()
        view.backgroundColor = .white10
        view.snp.makeConstraints { (make) in
            make.height.equalTo(1)
        }
        return view
    }
}
