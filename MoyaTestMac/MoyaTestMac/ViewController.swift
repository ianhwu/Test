//
//  ViewController.swift
//  MoyaTestMac
//
//  Created by Yan Hu on 2020/1/8.
//  Copyright © 2020 yan. All rights reserved.
//

import Cocoa
import Moya

class ViewController: NSViewController {
    let provider = MoyaProvider<Server>()
    override func viewDidLoad() {
        super.viewDidLoad()

        // Do any additional setup after loading the view.
        
        test()
    }
    
    func test() {
        DispatchQueue.global().async {
            self.provider.request(.get) { (result) in
                switch result {
                case let .success(moyaResponse):
                    do {
                        let person = try moyaResponse.map(Person.self)
                        print(person.name!, person.age!)
                    } catch {
                        print(error)
                    }
                    let data = moyaResponse.data // Data, your JSON response is probably in here!
                    let statusCode = moyaResponse.statusCode // Int - 200, 401, 500, etc
                    if let json = try? JSONSerialization.jsonObject(with: data, options: []) as? [String: Any] {
                        print(json)
                    }
                    print(statusCode)
                case let .failure(error):
                    // TODO: handle the error == best. comment. ever.
                    print(error)
                }
            }
        }
    }

    override var representedObject: Any? {
        didSet {
        // Update the view, if already loaded.
        }
    }


}


class Person: Codable {
    var name: String?
    var age: Int?
}

enum Server {
    case get
    case post
}

extension Server: TargetType {
    var baseURL: URL {
        return URL.init(string: "http://127.0.0.1:8181")!
    }
    
    var path: String {
        switch self {
        case .get:
            return "/get"
        case .post:
            return "/post"
        }
    }
    
    var method: Moya.Method {
        return .get
    }
    
    var sampleData: Data {
        return Data()
    }
    
    var task: Task {
        return .requestPlain
    }
    
    var headers: [String : String]? {
        return nil
    }
    
    public var validationType: ValidationType {
        switch self {
        case .get:
            return .successCodes
        default:
            return .none
        }
    }
    
}
