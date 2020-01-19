//
//  Os_unfair_lock_test.swift
//  Lock
//
//  Created by Yan Hu on 2020/1/17.
//  Copyright © 2020 cn.com.yan. All rights reserved.
//

import Foundation

func test_os_unfair_lock() {
    let lock = os_unfair_lock_t.allocate(capacity: 1)

    var number = 0
    DispatchQueue.global().async {
        os_unfair_lock_lock(lock)
        while number < 20 {
            number += 1
            print("os_unfair_lock in \(number)")
        }
        os_unfair_lock_unlock(lock)
    }

    DispatchQueue.global().async {
        os_unfair_lock_lock(lock)
        while number < 20 {
            number += 1
            print("os_unfair_lock outside \(number)")
        }
        os_unfair_lock_unlock(lock)
    }

    sleep(1)
    print("test_os_unfair_lock end \n\n\n")
}
