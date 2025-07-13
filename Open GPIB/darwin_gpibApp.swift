//
//  darwin_gpibApp.swift
//  darwin-gpib
//
//  Created by Thiago Mattos on 12/07/25.
//

import SwiftUI

@main
struct darwin_gpibApp: App {
    @Environment(\.openWindow) private var openWindow
    
    var body: some Scene {
        DevicesWindow()
        .commands {
            CommandGroup(replacing: CommandGroupPlacement.appInfo) {
                Button(action: {
                    openWindow(id: "about")
                }, label: {
                    Text("About Open GPIB")
                })
            }
        }
        
        OpenGPIBAboutWindow()
        
        
    }
}
