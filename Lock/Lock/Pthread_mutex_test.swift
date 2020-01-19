//
//  pthread_mutex_test.swift
//  Lock
//
//  Created by Yan Hu on 2020/1/17.
//  Copyright © 2020 cn.com.yan. All rights reserved.
//

import Foundation

func test_pthread_mutex() {
    var lock = pthread_mutex_t()
    pthread_mutex_init(&lock, nil)
    var number = 0
    DispatchQueue.global().async {
        pthread_mutex_lock(&lock)
        while number < 100 {
            number += 1
            print("pthread_mutex in \(number)")
        }
        pthread_mutex_unlock(&lock)
    }

    DispatchQueue.global().async {
        pthread_mutex_lock(&lock)
        while number < 100 {
            number += 1
            print("pthread_mutex outside \(number)")
        }
        pthread_mutex_unlock(&lock)
    }

    sleep(1)
    print("test_pthread_mutex end")
}
