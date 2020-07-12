//
//  AAA.swift
//  ModuleFramework
//
//  Created by yan on 2020/7/12.
//  Copyright © 2020 yan. All rights reserved.
//

import Foundation
import ModuleFramework.OtherFile

class AAA {
    func aaa() {
        let a = Test()
        a.runn()
        
        let b = Test2()
        b.runn2()
        
        let p = file()
        printFile(p)
        
        run()
        subRun()
        subSubRun()
        file3Run()
        
        let tSub = TestSubSub()
        tSub.testSubSubRun()
    }
}
