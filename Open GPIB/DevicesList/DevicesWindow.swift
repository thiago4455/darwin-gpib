//
//  DevicesListWindow.swift
//  Open GPIB
//
//  Created by Thiago Mattos on 13/07/25.
//

import SwiftUI
import AboutWindow

public struct DevicesWindow: Scene {
    @Environment(\.colorScheme)
    private var colorScheme
    
    var isMacOS26: Bool {
        if #available(macOS 26, *) {
            return true
        } else {
            return false
        }
    }
    
    public var body: some Scene {
        Window("Welcome To \(Bundle.displayName)", id: "welcome") {
            DevicesWindowView()
            .frame(width: 740, height: isMacOS26 ? 460 - 28 : 460)
            .task {
                if let window = NSApp.findWindow("welcome") {
                    window.styleMask.insert(.fullSizeContentView)
                    window.standardWindowButton(.closeButton)?.isHidden = true
                    window.standardWindowButton(.miniaturizeButton)?.isHidden = true
                    window.standardWindowButton(.zoomButton)?.isHidden = true
                    window.backgroundColor = .clear
                    window.isMovableByWindowBackground = true
                }
            }
        }
        .windowResizability(.contentSize)
        .windowStyle(.hiddenTitleBar)
    }
}


public struct OpenGPIBAboutWindow: Scene {
    
    var isMacOS26: Bool {
        if #available(macOS 26, *) {
            return true
        } else {
            return false
        }
    }
    
    @Environment(\.openURL) private var openURL
    private var appVersion: String { Bundle.versionString ?? "" }
    private var appBuild: String { Bundle.buildString ?? "" }
    private var appVersionPostfix: String { Bundle.versionPostfix ?? "" }
    
    public var body: some Scene {
        AboutWindow(
            subtitleView: {
                VStack(spacing: 10){
                    Text("for macOS")
                        .font(.system(size: 16, weight: .regular))
                        .foregroundColor(.primary)
                    VStack(alignment: .center, ) {
                        
                        
                        Text("darwin-gpib")
                        .textSelection(.enabled)
                        Text("Version \(appVersion)\(appVersionPostfix) (\(appBuild))")
                            .textSelection(.enabled)
                    }
                }
                
            },
            actions: {
                AboutButton(title: "GitHub", action: {
                    openURL(URL(string: "https://github.com/thiago4455/darwin-gpib")!)
                })
            },
            footer: {
                FooterView(
                    primaryView: {
                        Link(destination: URL(string: "https://opensource.org/licenses/MIT")!) {
                            Text("MIT License")
                                .underline()
                        }
                    },
                    secondaryView: {
                        Text("© 2025 Thiago Mattos")
                    }
                )
            }
        )
    }
}
