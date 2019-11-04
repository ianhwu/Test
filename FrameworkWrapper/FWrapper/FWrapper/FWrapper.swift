//
//  FWrapper.swift
//  FWrapper
//
//  Created by Yan Hu on 2019/11/4.
//  Copyright © 2019 yan. All rights reserved.
//

import UIKit
import SDKSwift

@objc public class FWrapper: NSObject {
    @objc public static func sdkVersion() -> String {
        return SDKSwift.sdkVersion()
    }
}
