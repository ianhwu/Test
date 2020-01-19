
//
//  DispatchQueue_lock_test.swift
//  Lock
//
//  Created by Yan Hu on 2020/1/17.
//  Copyright © 2020 cn.com.yan. All rights reserved.
//

import Foundation

// sync
func dispatch_queue_sync_test() {
    let queue = DispatchQueue.init(label: "queue", qos: .default, attributes: .concurrent, autoreleaseFrequency: .workItem, target: nil)
    var number = 0
    
    DispatchQueue.global().async {
        DispatchQueue.global().async {
            queue.sync {
                while number < 20 {
                    number += 1
                    print("dispatch_queue_sync in \(number)")
                }
            }
        }
        
        DispatchQueue.global().async {
            queue.sync {
                while number < 20 {
                    number += 1
                    print("dispatch_queue_sync outside \(number)")
                }
            }
        }
    }
    
    sleep(1)
    print("dispatch_queue_sync_test end \n\n\n")
}

// barrier
func dispatch_queue_barrier_test() {
    let queue = DispatchQueue.init(label: "queue", qos: .default, attributes: .concurrent, autoreleaseFrequency: .inherit, target: nil)
    var number = 0
    
    DispatchQueue.global().async {
        queue.async(flags: .barrier) {
            print("11 some test")
            while number < 20 {
                number += 1
                print("dispatch_queue_barrier in \(number)")
            }
        }
        
        for i in 0 ..< 20 {
            queue.async {
                print("\(i) some test")
                sleep(1)
            }
        }
        
        queue.sync(flags: .barrier) {
            print("outside some test")
            while number < 20 {
                number += 1
                print("dispatch_queue_barrier outside \(number)")
            }
        }
        
        queue.async {
            print("barrier end test")
        }
        
        print("execute end")
    }
    
    sleep(4)
    print("dispatch_queue_barrier_test end \n\n\n")
}
