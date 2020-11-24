//
//  ViewController.swift
//  TestOpenCV
//
//  Created by Yan Hu on 2020/9/21.
//

import UIKit
import Photos
import MobileCoreServices
import Componet
import AVFoundation

class ViewController: UIViewController, UIImagePickerControllerDelegate & UINavigationControllerDelegate {
//    var imageView = CVWrapper.cameraView()
    var imageView = UIImageView()
    var scrollView = UIScrollView()
    var images = [UIImage]()
    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .white
        scrollView.addSubview(imageView)
        view.addSubview(scrollView)
        imageView.contentMode = .scaleAspectFit
        
        scrollView.frame = view.bounds
        imageView.frame = view.bounds
        
        let btn = UIButton()
        btn.backgroundColor = .red
        btn.frame = CGRect.init(x: 0, y: view.frame.height - 50, width: 50, height: 50)
        view.addSubview(btn)
        btn.addTarget(self, action: #selector(add), for: .touchUpInside)
        
        let btn1 = UIButton()
        btn1.backgroundColor = .red
        btn1.frame = CGRect.init(x: view.frame.width - 50, y: view.frame.height - 50, width: 50, height: 50)
        view.addSubview(btn1)
        btn1.addTarget(self, action: #selector(done), for: .touchUpInside)
    }
    
    @objc func done() {
        
        guard images.count > 1 else { return }
        DispatchQueue.global().async {
            var image = self.images[0]
            var imgs = [image]
            for i in 1 ..< self.images.count {
                let newImage = self.images[i]
                imgs.append(newImage)
                for image in imgs {
                    print(image.size)
                }
                var stitchedimage = CVWrapper.process(with: imgs)
                if stitchedimage.size == .zero {
                    stitchedimage = image
                }
                image = stitchedimage
                self.setImage(image: image)
                sleep(1)
                imgs = [image]
            }
            self.images = []
        }
    }
    
    func setImage(image: UIImage) {
        DispatchQueue.main.async {
            self.imageView.image = image
            let width = self.view.frame.width
            let height = image.size.height / image.size.width * width
            self.imageView.frame = CGRect.init(x: 0, y: 0, width: width, height: height)
            self.scrollView.contentSize = .init(width: width, height: height)
        }
    }
    
    @objc func add() {
        let vc = UIImagePickerController()
        vc.delegate = self
        vc.sourceType = .photoLibrary
        vc.mediaTypes = ["public.movie", "public.image"]
        present(vc, animated: true, completion: nil)
    }
    
    func imagePickerController(_ picker: UIImagePickerController, didFinishPickingMediaWithInfo info: [UIImagePickerController.InfoKey : Any]) {
        dismiss(animated: true, completion: nil)
        if let image = info[.originalImage] as? UIImage {
            images.append(image)
        } else if let url = info[.mediaURL] as? URL {
            stitch(url: url)
        }
    }
    
    func stitch(url: URL) {
        let asset = AVAsset(url: url) // link to some video
        let imageGenerator = AVAssetImageGenerator(asset: asset)
        let duration = asset.duration
        for second in 1 ... Int(duration.value / Int64(duration.timescale)) {
            for i in 0 ..< 2 {
                let value = second * Int(duration.timescale) + i * 300
                let screenshotTime = CMTime.init(value: CMTimeValue(value), timescale: 600)
                if let imageRef = try? imageGenerator.copyCGImage(at: screenshotTime, actualTime: nil) {
                    let image = UIImage(cgImage: imageRef)
                    images.append(image)
                    
//                    let width = view.frame.width
//                    let height = image.size.height / image.size.width * width
//                    images.append(image.reSizeImage(reSize: .init(width: width, height: height)))
                }
            }
//            let screenshotTime = CMTime(seconds: Double(second), preferredTimescale: 1)
//            if let imageRef = try? imageGenerator.copyCGImage(at: screenshotTime, actualTime: nil) {
//                let image = UIImage(cgImage: imageRef)
//                let width = view.frame.width
//                let height = image.size.height / image.size.width * width
//                images.append(image.reSizeImage(reSize: .init(width: width, height: height)))
//                images.append(image)
//            }
//            self.images.append(CVWrapper.process(with: images))
        }
        done()
    }
}

extension UIImage {
    /**
     *  重设图片大小
     */
    func reSizeImage(reSize: CGSize) -> UIImage {
        var reSizeImage: UIImage?
        autoreleasepool {
            UIGraphicsBeginImageContextWithOptions(reSize, false, scale) // 保持图片原来的scale
            draw(in: CGRect.init(origin: .zero, size: reSize));
            reSizeImage = UIGraphicsGetImageFromCurrentImageContext();
            UIGraphicsEndImageContext();
        }
        return reSizeImage!
    }
}

class SubView: UIView {
    override func layoutSubviews() {
        super.layoutSubviews()
        
    }
}
