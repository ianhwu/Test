//
//  main.swift
//  Lock
//
//  Created by Yan Hu on 2020/1/17.
//  Copyright © 2020 cn.com.yan. All rights reserved.
//

import Foundation

// 自旋锁会持续使用 cpu，知道获取了资源并且执行完
// 互斥锁会会切换上下文，变成不活跃状态，当获取资源后才重新占用 cpu

// 自旋锁
test_os_unfair_lock()

// 互斥锁 mutex
// The NSLock class uses POSIX threads to implement its locking behavior.
// NSLock, NSRecursiveLock, NSCondition, NSConditionLock 都是使用 pthread_mutex_t 实现的
test_pthread_mutex()

// 条件
test_pthread_cond()

// 递归锁
test_pthread_mutex_recursive()

// 使用信号量实现互斥 DispatchSemaphore, 当资源设置为 1 的时候，相当于互斥锁，
// 当信号量为 0 的时候, 会 sleep，知道资源为 1
test_semaphore()

/********以下内容主要针对并行队列*********/
// DispatchQueue sync + 串行队列也可以实现锁的效果
// sync 阻碍当前队列, 所以串行队列肯定被阻碍，串行队列只有一个线程
// sync 目标队列不会被阻碍，除非目标线程也是串行队列
// sync 在串行队列中使用，当目标线程也是当前线程，会造成死锁
dispatch_queue_sync_test()
// barrier 阻碍目标队列，串行队列 barrier 毫无用处
// sync barrier 阻碍当前队列和目标队列
// async barrier 阻碍目标队列，不会阻碍当前队列
dispatch_queue_barrier_test()


// 总结
// 针对 GCD 并发队列的同步处理，可以使用 sync 和 barrier
// 针对 线程 处理可以跟进需求使用不同的锁来处理

