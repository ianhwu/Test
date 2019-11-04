//
//  ViewController.swift
//  SwiftTest
//
//  Created by Yan Hu on 2019/10/16.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit

@propertyWrapper
struct RGBColorWrapper {
    private var r: CGFloat
    private var g: CGFloat
    private var b: CGFloat
    private var alpha: CGFloat
    
    var wrappedValue: UIColor { UIColor.init(red: r, green: g, blue: b, alpha: alpha) }
    
    init(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat, _ alpha: CGFloat = 1) {
        self.r = r / 255
        self.g = g / 255
        self.b = b / 255
        self.alpha = alpha
    }
}

struct Color {
    @RGBColorWrapper(255, 0, 0)
    static var redRed: UIColor
}


class ViewController: UIViewController {

    override func viewDidLoad() {
        super.viewDidLoad()
        // Do any additional setup after loading the view.
        view.backgroundColor = .white
        
//        let scrollView = UIScrollView()
//        view.addSubview(scrollView)
//        scrollView.frame = view.bounds
//        
//        let imageView = UIImageView()
//        scrollView.addSubview(imageView)
//
//        imageView.frame = CGRect.init(origin: .zero, size: image.size)
//        scrollView.contentSize = image.size
//        imageView.image = image
        
    }
}

func resizedImage(at url: URL, for size: CGSize) -> UIImage? {
    guard let image = UIImage(contentsOfFile: url.path) else {
        return nil
    }

    let renderer = UIGraphicsImageRenderer(size: size)
    return renderer.image { (context) in
        image.draw(in: CGRect(origin: .zero, size: size))
    }
}

