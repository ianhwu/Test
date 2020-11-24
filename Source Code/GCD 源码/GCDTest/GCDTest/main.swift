//
//  main.swift
//  GCDTest
//
//  Created by Yan Hu on 2019/12/17.
//  Copyright © 2019 yan. All rights reserved.
//

import Foundation

class CustomOperation: Operation {
    override func main() {
        while !isCancelled {
            sleep(1)
            print("Custom Operation Run")
        }
    }
}


var source: DispatchSourceTimer?
func test() {
    let concurrentQueue = DispatchQueue.init(label: "concurrent", qos: .default, attributes: .concurrent, autoreleaseFrequency: .inherit, target: nil)
    let concurrentQueue1 = DispatchQueue.init(label: "concurrent1", qos: .default, attributes: .concurrent, autoreleaseFrequency: .inherit, target: nil)
    let serialQueue = DispatchQueue.init(label: "serial")
    
    
    let semaphore = DispatchSemaphore.init(value: 1)
    concurrentQueue.async {
        semaphore.wait()
        sleep(5)
        print("first end")
        semaphore.signal()
    }
    
    concurrentQueue.async {
        semaphore.wait()
        print("second end")
        semaphore.signal()
    }
    
    concurrentQueue.async {
        
    }
    
    source = DispatchSource.makeTimerSource(flags: [], queue: concurrentQueue)
    source?.schedule(deadline: .now(), repeating: .seconds(1), leeway: .milliseconds(500))
    source?.setEventHandler {
        print(Date())
    }
    source?.resume()
    
//    print("end")
}

test()
sleep(20)
