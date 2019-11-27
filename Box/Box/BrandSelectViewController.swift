//
//  BrandSelectViewController.swift
//  Box
//
//  Created by Yan Hu on 2019/11/27.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit

class BrandSelectViewController: BaseViewController {

    override func viewDidLoad() {
        super.viewDidLoad()

        // Do any additional setup after loading the view.
        title = "Brand Select"
        navBar.backgroundColor = .purple
        view.backgroundColor = .modena
        
        let margin: CGFloat = 18
        let width = (view.width - margin * 3) / 2
        
        let abbottView = BrandSelectItem()
        abbottView.imageName = "brand-select-abbott"
        abbottView.title = "Abbott"
        view.addSubview(abbottView)
        
        abbottView.snp.makeConstraints { (make) in
            make.left.equalTo(margin)
            make.width.equalTo(width)
            make.top.equalTo(navBar.snp.bottom).offset(margin)
        }
        
        let dexcomView = BrandSelectItem()
        dexcomView.imageName = "brand-select-dexcom"
        dexcomView.title = "Dexcom"
        view.addSubview(dexcomView)
        
        dexcomView.snp.makeConstraints { (make) in
            make.left.equalTo(abbottView.snp.right).offset(margin)
            make.width.equalTo(width)
            make.top.equalTo(abbottView)
        }
        
        let medtronicView = BrandSelectItem()
        medtronicView.imageName = "brand-select-medtronic"
        medtronicView.title = "Medtronic"
        view.addSubview(medtronicView)
        
        medtronicView.snp.makeConstraints { (make) in
            make.left.equalTo(abbottView)
            make.width.equalTo(width)
            make.top.equalTo(abbottView.snp.bottom).offset(margin)
        }
        
        let rocheView = BrandSelectItem()
        rocheView.imageName = "brand-select-roche"
        rocheView.title = "Roche Diagnostics"
        view.addSubview(rocheView)
        
        rocheView.snp.makeConstraints { (make) in
            make.left.equalTo(abbottView.snp.right).offset(margin)
            make.width.equalTo(width)
            make.top.equalTo(medtronicView)
        }
        
        let echoView = BrandSelectItem()
        echoView.imageName = "brand-select-echo"
        echoView.title = "Echo Therapeautiss"
        view.addSubview(echoView)
        
        echoView.snp.makeConstraints { (make) in
            make.left.equalTo(abbottView)
            make.width.equalTo(width)
            make.top.equalTo(medtronicView.snp.bottom).offset(margin)
        }
        
        let otherView = BrandSelectItem()
        otherView.imageName = "brand-select-roche"
        otherView.title = "Other Devices"
        view.addSubview(otherView)
        
        otherView.snp.makeConstraints { (make) in
            make.left.equalTo(abbottView.snp.right).offset(margin)
            make.width.equalTo(width)
            make.top.equalTo(echoView)
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

class BrandSelectItem: UIView {
    lazy private var imageView: UIImageView = {
        let view = UIImageView.scaleAspectFitImageView()
        return view
    }()
    
    var imageName: String? {
        didSet {
            guard let name = imageName else { return }
            imageView.image = UIImage.init(named: name)
        }
    }
    
    lazy private var titleLabel: UILabel = {
        let label = UILabel.white12()
        label.textAlignment = .center
        return label
    }()
    
    var title: String? {
        didSet {
            titleLabel.text = title
        }
    }
    
    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .purple
        cornerRadius = cornerRadiusSmall
        addSubview(imageView)
        addSubview(titleLabel)
        
        imageView.snp.makeConstraints { (make) in
            make.centerY.equalTo(self).offset(-10)
            make.centerX.equalTo(self)
            make.width.height.equalTo(50)
        }
        
        titleLabel.snp.makeConstraints { (make) in
            make.top.equalTo(imageView.snp.bottom)
            make.centerX.equalTo(self)
        }
        
        snp.makeConstraints { (make) in
            make.height.equalTo(100)
        }
    }
    
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }
}
