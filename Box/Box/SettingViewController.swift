//
//  SettingViewController.swift
//  Box
//
//  Created by Yan Hu on 2019/11/25.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit
import SwifterSwift

class SettingViewController: BaseViewController {
    lazy private var iconView: UIImageView = {
        let imageView = UIImageView()
        imageView.image = UIImage.init(named: "another-person")
        return imageView
    }()
    
    lazy private var nameLabel: UILabel = {
        let label = UILabel()
        label.font = .font24
        label.text = "Another User"
        label.textColor = .white
        return label
    }()
    override func viewDidLoad() {
        super.viewDidLoad()
        // Do any additional setup after loading the view.
        
        setupView()
    }
    
    private func setupView() {
        view.addSubview(iconView)
        view.addSubview(nameLabel)
        
        let contentView = UIView()
        contentView.backgroundColor = .modena
        contentView.cornerRadius = cornerRadiusLarge
        view.addSubview(contentView)
        
        iconView.snp.makeConstraints { (make) in
            make.left.equalTo(margin + margin2)
            make.width.height.equalTo(iconLength1)
            make.top.equalTo(statusBarHeight + space)
        }
        
        nameLabel.snp.makeConstraints { (make) in
            make.centerY.equalTo(iconView)
            make.left.equalTo(iconView.snp.right).offset(margin)
        }
        
        contentView.snp.makeConstraints { (make) in
            make.top.equalTo(iconView.snp.bottom).offset(space)
            make.left.equalTo(margin2)
            make.right.equalTo(-margin2)
        }
        
        let cell1 = SettingCell()
        cell1.icon = UIImage.init(named: "outline-reorder")
        cell1.title = "Settings"
        contentView.addSubview(cell1)
        cell1.didSelect = {
            [weak self] in
        }
        
        cell1.snp.makeConstraints { (make) in
            make.left.right.equalTo(contentView)
            make.top.equalTo(contentView)
        }
        
        let cell2 = SettingCell()
        cell2.icon = UIImage.init(named: "outline-reorder")
        cell2.title = "i-CGM"
        contentView.addSubview(cell2)
        cell2.didSelect = {
            [weak self] in
            let vc = BrandSelectViewController()
            self?.navigationController?.pushViewController(vc, animated: true)
        }
        
        cell2.snp.makeConstraints { (make) in
            make.left.right.equalTo(contentView)
            make.top.equalTo(cell1.snp.bottom)
        }
        
        let cell3 = SettingCell()
        cell3.icon = UIImage.init(named: "outline-reorder")
        cell3.title = "integration"
        contentView.addSubview(cell3)
        cell3.didSelect = {
            [weak self] in
        }
        
        cell3.snp.makeConstraints { (make) in
            make.left.right.equalTo(contentView)
            make.top.equalTo(cell2.snp.bottom)
        }
        
        let cell4 = SettingCell()
        cell4.icon = UIImage.init(named: "outline-reorder")
        contentView.addSubview(cell4)
        cell4.title = "About"
        cell4.didSelect = {
            [weak self] in
            let vc = AboutViewController()
            self?.navigationController?.pushViewController(vc, animated: true)
        }
        
        cell4.snp.makeConstraints { (make) in
            make.left.right.equalTo(contentView)
            make.top.equalTo(cell3.snp.bottom)
        }
        
        contentView.snp.makeConstraints { (make) in
            make.bottom.equalTo(cell4)
        }
    }
    
    /*
    // MARK: - Navigation

    // In a storyboard-based application, you will often want to do a little preparation before navigation
    override func prepare(for segue: UIStoryboardSegue, sender: Any?) {
        // Get the new view controller using segue.destination.
        // Pass the selected object to the new view controller.
    }
    */

}

class SettingCell: UIView {
    lazy private var iconView: UIImageView = {
        let imageView = UIImageView()
        imageView.contentMode = .scaleAspectFit
        return imageView
    }()
    
    lazy private var titleLabel: UILabel = {
        let label = UILabel()
        label.font = .font16
        label.textColor = .white
        return label
    }()
    
    lazy private var arrowView: UIImageView = {
        let imageView = UIImageView()
        imageView.image = UIImage.init(named: "arrow-right")
        return imageView
    }()
    
    lazy private var topLineView: UIView = {
        let view = UIView()
        view.backgroundColor = .white10
        return view
    }()
    
    var icon: UIImage? {
        didSet {
            iconView.image = icon
        }
    }
    
    var title: String? {
        didSet {
            titleLabel.text = title
        }
    }
    
    var topLineIsHidden = false {
        didSet {
            topLineView.isHidden = topLineIsHidden
        }
    }
    
    var arrowIsHidden = false {
        didSet {
            arrowView.isHidden = arrowIsHidden
        }
    }
    
    var didSelect: (() -> ())?
    
    override init(frame: CGRect) {
        super.init(frame: frame)
        setupView()
        
        addTapGesture { [weak self] _ in
            self?.didSelect?()
        }
    }
    
    private func setupView() {
        addSubview(iconView)
        addSubview(titleLabel)
        addSubview(topLineView)
        addSubview(arrowView)
        
        topLineView.snp.makeConstraints { (make) in
            make.top.equalTo(self)
            make.left.equalTo(margin)
            make.right.equalTo(-margin)
            make.height.equalTo(lineHeight)
        }
        
        iconView.snp.makeConstraints { (make) in
            make.width.height.equalTo(iconLength)
            make.left.equalTo(margin)
            make.centerY.equalTo(self)
        }
        
        titleLabel.snp.makeConstraints { (make) in
            make.centerY.equalTo(self)
            make.left.equalTo(iconView.snp.right).offset(gap)
        }
        
        arrowView.snp.makeConstraints { (make) in
            make.right.equalTo(-margin)
            make.centerY.equalTo(self)
        }
        
        snp.makeConstraints { (make) in
            make.height.equalTo(itemHeight)
        }
    }
    
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }
}
