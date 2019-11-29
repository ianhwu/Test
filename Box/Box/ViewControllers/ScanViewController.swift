//
//  ScanViewController.swift
//  Box
//
//  Created by Yan Hu on 2019/11/29.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit
import CoreBluetooth
class BluetoothPeripheral: NSObject {
    var mac: String?
    var peripheral: CBPeripheral?
    var hardware: String?
    var firmware: String?
}

class ScanViewController: BaseViewController {
    var list = [BluetoothPeripheral]() {
        didSet {
            tableView.reloadData()
        }
    }
    
    lazy private var loadingView: LoadingView = {
        let loadingView = LoadingView()
        loadingView.isHidden = true
        return loadingView
    }()
    
    private var bubbleView = UIImageView.view(with: "bubble")
    
    lazy private var scanBtn: UIButton = {
        let btn = UIButton.pink("Scan")
        btn.addTarget(self, action: #selector(scan), for: .touchUpInside)
        return btn
    }()
    
    lazy private var descriptionLabel: UILabel = {
        let label = UILabel.value()
        label.text = "Device not found, please make sure your Bubble fully charged and keep it as close as to your phone"
        label.isHidden = true
        return label
    }()
    
    lazy private var tableView: UITableView = {
        let table = UITableView.init(frame: .zero, style: .plain)
        table.delegate = self
        table.dataSource = self
        table.rowHeight = Config.cellHeight
        table.backgroundColor = .clear
        table.separatorStyle = .none
        table.register(UITableViewCell.self, forCellReuseIdentifier: "cell")
        return table
    }()
    
    @objc private func scan() {
        guard scanBtn.title(for: .normal) == "Scan" else { return }
        bubbleView.isHidden = true
        loadingView.isHidden = false
        scanBtn.unSelected("Scanning...")
        loadingView.startAnimation()
        
        let _ = Timer.scheduledTimer(withTimeInterval: 5, repeats: false) { [weak self] _ in
            if self?.list.isEmpty ?? false {
                self?.scanBtn.pink("Scan")
                self?.descriptionLabel.isHidden = false
            }
        }
    }
    
    override func viewDidLoad() {
        super.viewDidLoad()
        // Do any additional setup after loading the view.
        title = "Scan Device"
        
        view.addSubview(loadingView)
        view.addSubview(scanBtn)
        view.addSubview(descriptionLabel)
        view.addSubview(tableView)
        view.addSubview(bubbleView)
        
        loadingView.snp.makeConstraints { (make) in
            make.top.equalTo(navBar.snp.bottom).offset(50)
            make.centerX.equalTo(view)
            make.width.height.equalTo(120)
        }
        
        bubbleView.snp.makeConstraints { (make) in
            make.center.equalTo(loadingView)
        }
        
        descriptionLabel.snp.makeConstraints { (make) in
            make.left.equalTo(50)
            make.right.equalTo(-50)
            make.top.equalTo(loadingView.snp.bottom).offset(margin)
        }
        
        scanBtn.snp.makeConstraints { (make) in
            make.left.equalTo(margin)
            make.right.equalTo(-margin)
            make.top.equalTo(descriptionLabel.snp.bottom).offset(margin)
        }
        
        tableView.snp.makeConstraints { (make) in
            make.top.equalTo(scanBtn.snp.bottom).offset(margin)
            make.left.right.equalTo(view)
            make.bottom.equalTo(-margin)
        }
    }
}

extension ScanViewController: UITableViewDelegate, UITableViewDataSource {
    func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        return list.count
    }
    
    func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(withIdentifier: "cell", for: indexPath)
        cell.backgroundColor = .modena
        cell.textLabel?.textColor = .white
        cell.textLabel?.text = list[indexPath.row].mac
        return cell
    }
    
    func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
//        cgmTransmitter?.connect(to: list[indexPath.row])
        navigationController?.popViewController(animated: true)
    }
}

private class LoadingView: UIView, CAAnimationDelegate {
    private let imageView = UIImageView.view(with: "bluetooth")
    private var shape = CAShapeLayer()
    private var lineWidth: CGFloat = 4
    override init(frame: CGRect) {
        super.init(frame: frame)
        layer.addSublayer(shape)
        shape.fillColor = UIColor.clear.cgColor
        shape.strokeStart = 0
        shape.strokeEnd = 0
        shape.strokeColor = UIColor.blue.cgColor
        shape.lineCap = .round
        shape.lineWidth = lineWidth
        
        addSubview(imageView)
        imageView.snp.makeConstraints { (make) in
            make.center.equalTo(self)
        }
    }
    
    override func layoutSubviews() {
        super.layoutSubviews()
        let radius = min(width / 2, height / 2) - lineWidth / 2
        let path = UIBezierPath.init(arcCenter: .init(x: width / 2, y: height / 2), radius: radius, startAngle: -(.pi / 2), endAngle: .pi * 1.5, clockwise: true)
        shape.path = path.cgPath
    }
    
    func startAnimation() {
        shape.removeAllAnimations()
        shape.isHidden = false
        let animation = CABasicAnimation.init(keyPath: "strokeEnd")
        animation.delegate = self
        animation.fromValue = 0
        animation.toValue = 1
        animation.duration = 3
        animation.repeatCount = 10000000
        animation.timingFunction = CAMediaTimingFunction.init(name: .linear)
        shape.add(animation, forKey: "strokeEnd")
    }
    
    func stopAnimation() {
        shape.isHidden = true
    }
 
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }
}
