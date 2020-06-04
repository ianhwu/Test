//
//  ContentView.swift
//  SwiftUILearning
//
//  Created by Yan Hu on 2020/5/18.
//  Copyright © 2020 cn.com.yan. All rights reserved.
//

import SwiftUI

struct ContentView: View {
    @State var animate = false
    @State private var selected = 1
    @State var vibrateOnRing = true
    @EnvironmentObject var model: ViewModel
    
    var animation = Animation.easeIn
    
    var body: some View {
        NavigationView {
            VStack(alignment: .center) {
                WebView(urlType: .localUrl, viewModel: model)
                    .background(Color.blue)
                    .border(Color.green, width: 1)
                NavigationLink(destination: CircleView()) {
                    Text("CircleView")
                }
                Button("Sign In", action: {
                    self.model.valuePublisher.send(["title": "%%\'\"\\\\v\t\r\\f\n\\b"])
                })
                Spacer(minLength: 50)
            }.navigationBarTitle("Title").background(Color.red).edgesIgnoringSafeArea(.bottom)
        }
    }
}

struct CircleView: View {
    var body: some View {
        Circle()
            .fill(Color.green)
            .frame(width: 25, height: 25, alignment: .center).navigationBarTitle("Run")
    }
}

struct ContentView_Previews: PreviewProvider {
    static var previews: some View {
        ContentView()
    }
}
