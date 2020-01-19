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
        while number < 20 {
            number += 1
            print("pthread_mutex in \(number)")
        }
        pthread_mutex_unlock(&lock)
    }

    DispatchQueue.global().async {
        pthread_mutex_lock(&lock)
        while number < 20 {
            number += 1
            print("pthread_mutex outside \(number)")
        }
        pthread_mutex_unlock(&lock)
    }

    sleep(1)
    print("test_pthread_mutex end \n\n\n")
}

/// 条件 相当于 NSCondition 和 NSConditionLock 的使用
/// lock -> wait -> unlock
/// lock -> signal -> unlock
func test_pthread_cond() {
    var cond = pthread_cond_t()
    pthread_cond_init(&cond, nil)
    var mutex = pthread_mutex_t()
    pthread_mutex_init(&mutex, nil)
    var number = 0
    DispatchQueue.global().async {
        pthread_mutex_lock(&mutex)
        pthread_cond_wait(&cond, &mutex)
        while number < 20 {
            number += 1
            print("pthread_cond in \(number)")
        }
        pthread_mutex_unlock(&mutex)
    }

    DispatchQueue.global().async {
        sleep(1)
        pthread_mutex_lock(&mutex)
        while number < 20 {
            number += 1
            print("pthread_cond outside \(number)")
        }
        pthread_cond_signal(&cond)
        pthread_mutex_unlock(&mutex)
    }

    sleep(2)
    print("test_pthread_cond end \n\n\n")
}

func test_condition() {
    let condition = NSCondition()
    var number = 0
    DispatchQueue.global().async {
        condition.lock()
        condition.wait()
        while number < 20 {
            number += 1
            print("condition in \(number)")
        }
        condition.unlock()
    }

    DispatchQueue.global().async {
        condition.lock()
        sleep(1)
        while number < 20 {
            number += 1
            print("condition outside \(number)")
        }
        condition.signal()
        condition.unlock()
    }

    sleep(2)
    print("test_condition end \n\n\n")
}

func test_condition_lock() {
    let condition = NSConditionLock(condition: 1)
    var number = 0
    DispatchQueue.global().async {
        condition.lock(whenCondition: 1)
        sleep(1)
        while number < 20 {
            number += 1
            print("condition_lock in \(number)")
        }
        condition.unlock(withCondition: 2)
    }

    DispatchQueue.global().async {
        condition.lock(whenCondition: 2)
        while number < 20 {
            number += 1
            print("condition_lock outside \(number)")
        }
        condition.unlock(withCondition: 3)
    }

    sleep(2)
    print("test_condition_lock end \n\n\n")
}

/// 递归锁
func test_pthread_mutex_recursive() {
    /// lock1 为递归锁，lock2 为默认的锁
    var lock1 = pthread_mutex_t()
    var attr = pthread_mutexattr_t()
    pthread_mutexattr_init(&attr)
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE)
    pthread_mutex_init(&lock1, &attr)
    pthread_mutexattr_destroy(&attr)
    var number1 = 0
    func run1() {
        pthread_mutex_lock(&lock1)
        if number1 < 20 {
            number1 += 1
            print("pthread_mutex_recursive lock1 \(number1)")
            run1()
        }
        pthread_mutex_unlock(&lock1)
    }
    
    var lock2 = pthread_mutex_t()
    pthread_mutex_init(&lock2, nil)
    var number2 = 0
    func run2() {
        pthread_mutex_lock(&lock2) // 死锁在这里
        if number2 < 20 {
            number2 += 1
            print("pthread_mutex_recursive lock2 \(number2)")
            run2()
        }
        pthread_mutex_unlock(&lock2)
    }
    
    DispatchQueue.global().async {
        run1()
        run2()
    }
    
    sleep(1)
    print("test_pthread_mutex_recursive end \n\n\n")
}
