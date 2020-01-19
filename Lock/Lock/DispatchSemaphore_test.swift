//
//  DispatchSemaphore_test.swift
//  Lock
//
//  Created by Yan Hu on 2020/1/17.
//  Copyright © 2020 cn.com.yan. All rights reserved.
//

import Foundation

func test_semaphore() {
    let semaphore = DispatchSemaphore(value: 1)
    var number = 0
    DispatchQueue.global().async {
        semaphore.wait()
        while number < 20 {
            number += 1
            print("semaphore in \(number)")
        }
        semaphore.signal()
    }

    DispatchQueue.global().async {
        semaphore.wait()
        while number < 20 {
            number += 1
            print("semaphore outside \(number)")
        }
        semaphore.signal()
    }

    sleep(1)
    print("test_semaphore end \n\n\n")
}
