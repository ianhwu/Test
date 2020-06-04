//
//  main.swift
//  CryptoTest
//
//  Created by Yan Hu on 2020/6/4.
//  Copyright © 2020 cn.com.yan. All rights reserved.
//

import Foundation
import CommonCrypto
import CryptoKit

// Defines types of hash string outputs available
public enum HashOutputType {
    // standard hex string output lowercased
    case hexLowercased
    // standard hex string output uppercased
    case hexUppercased
    // base 64 encoded string output
    case base64
}

// Defines types of hash algorithms available
public enum HashType {
    case md5
    case sha1
    case sha224
    case sha256
    case sha384
    case sha512

    var length: Int32 {
        switch self {
        case .md5: return CC_MD5_DIGEST_LENGTH
        case .sha1: return CC_SHA1_DIGEST_LENGTH
        case .sha224: return CC_SHA224_DIGEST_LENGTH
        case .sha256: return CC_SHA256_DIGEST_LENGTH
        case .sha384: return CC_SHA384_DIGEST_LENGTH
        case .sha512: return CC_SHA512_DIGEST_LENGTH
        }
    }
}

public extension String {

    /// Hashing algorithm for hashing a string instance.
    ///
    /// - Parameters:
    ///   - type: The type of hash to use.
    ///   - output: The type of output desired, defaults to .hex.
    /// - Returns: The requested hash output or nil if failure.
    func hashed(_ type: HashType, output: HashOutputType = .hexLowercased) -> String? {

        // convert string to utf8 encoded data
        guard let message = data(using: .utf8) else { return nil }
        return message.hashed(type, output: output)
    }
}

extension Data {
    // From Stackoverflow, see https://stackoverflow.com/questions/39075043/how-to-convert-data-to-hex-string-in-swift
    /// conert to hexencoded string
    public func hexString1(_ lowercased: Bool = true) -> String {
        let hexAlphabet = (lowercased ? "0123456789abcdef" : "0123456789ABCDEF").unicodeScalars.map { $0 }
        return String(reduce(into: "".unicodeScalars, { (result, value) in
            result.append(hexAlphabet[Int(value/16)])
            result.append(hexAlphabet[Int(value%16)])
        }))
    }
    
    // It's not as elegant, but it's about 10 times faster than function hexString1
    /// conert to hexencoded string
    public func hexString2(_ lowercased: Bool = true) -> String {
        let format = lowercased ? "%02hhx" : "%02hhX"
        return map { String(format: format, $0) }.joined()
    }
    
    /// Hashing algorithm for hashing a Data instance.
    ///
    /// - Parameters:
    ///   - type: The type of hash to use.
    ///   - output: The type of hash output desired, defaults to .hex.
    ///   - Returns: The requested hash output or nil if failure.
    public func hashed(_ type: HashType, output: HashOutputType = .hexLowercased) -> String {
        if #available(iOS 13.0, OSX 10.15, watchOS 6.0, tvOS 13.0, *),
            type == .md5 {
            var md5 = Insecure.MD5()
            md5.update(data: self)
            let digest = md5.finalize().withUnsafeBytes { Data($0) }
            
            // return the value based on the specified output type.
            switch output {
            case .hexLowercased: return digest.hexString2()
            case .hexUppercased: return digest.hexString2(false)
            case .base64: return digest.base64EncodedString()
            }
        }
        
        // setup data variable to hold hashed value
        var digest = Data(count: Int(type.length))
        _ = digest.withUnsafeMutableBytes { (digestBytes) -> Bool in
            withUnsafeBytes({ (messageBytes) -> Bool in
                let length = CC_LONG(self.count)
                let messageAddress = messageBytes.baseAddress
                let digestAddress = digestBytes.bindMemory(to: UInt8.self).baseAddress
                switch type {
                case .md5:
                    if #available(iOS 13.0, OSX 10.15, watchOS 6.0, tvOS 13.0, *) {
                        return true
                    } else {
                        CC_MD5(messageAddress, length, digestAddress)
                    }
                case .sha1: CC_SHA1(messageAddress, length, digestAddress)
                case .sha224: CC_SHA224(messageAddress, length, digestAddress)
                case .sha256: CC_SHA256(messageAddress, length, digestAddress)
                case .sha384: CC_SHA384(messageAddress, length, digestAddress)
                case .sha512: CC_SHA512(messageAddress, length, digestAddress)
                }
                return true
            })
        }

        // return the value based on the specified output type.
        switch output {
        case .hexLowercased: return digest.hexString2()
        case .hexUppercased: return digest.hexString2(false)
        case .base64: return digest.base64EncodedString()
        }
    }
}

print("asdfasdfasdf".hashed(.md5)!)

print("asdfasdfasdf".hashed(.md5, output: .base64)!)

print("asdfasdfasdf".hashed(.md5, output: .hexUppercased)!)

print("asdfasdfasdf".hashed(.sha1)!)

print("asdfasdfasdf".hashed(.sha1, output: .base64)!)

print("asdfasdfasdf".hashed(.sha1, output: .hexUppercased)!)
