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

    init() {
        // Tried, and reverted: auto-firing ExtensionManager.shared.activate()
        // here on every Debug launch (the same call "Reconnect all adaptors"
        // makes) did let a rebuilt dext install with no physical replug —
        // confirmed working, more than once. But relaunching the app is also
        // how a rebuild gets installed during normal iteration, and doing
        // this on *every* one of those relaunches reactivates the extension
        // far faster than macOS settles each swap, which reliably reproduced
        // the stuck "[terminating for upgrade via delegate]" ghost extension
        // state — worse than the replug it was meant to save. Removed rather
        // than rate-limited: "Reconnect all adaptors" already does this
        // deliberately, one click at a time, which is the pace that actually
        // works.
    }

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
