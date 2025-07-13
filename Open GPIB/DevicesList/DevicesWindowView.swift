//
//  DevicesWindowView.swift
//  Open GPIB
//
//  Created by Thiago Mattos on 13/07/25.
//

import SwiftUI

public enum FocusTarget: Hashable {
    case none
    case devices
    case dismissButton
    case action1
    case action2
    case action3
}

public struct DevicesWindowView: View{
    @Environment(\.dismiss)
    private var dismissWindow

    @Environment(\.colorScheme)
    private var colorScheme

    @FocusState private var focusedField: FocusTarget?

    @State private var devices: [URL] = [URL]()  //Get devices here
    @State private var selection: Set<URL> = []
    
    
    public var body: some View {
        let dismiss = dismissWindow.callAsFunction

        return HStack(spacing: 0) {
            WelcomeView(
                dismissWindow: dismiss,
                focusedField: $focusedField
            )

            Group {
                DevicesListView(
                    devices: $devices,
                    selection: $selection,
                    focusedField: $focusedField,
                    dismissWindow: dismiss
                )
            }
            .listStyle(.sidebar)
            .scrollContentBackground(.hidden)
            .background {
                if colorScheme == .dark {
                    Color(.black).opacity(0.075)
                        .background(.thickMaterial)
                } else {
                    Color(.white).opacity(0.6)
                        .background(.regularMaterial)
                }
            }
        }
        .cursor(.current)
        .edgesIgnoringSafeArea(.top)
        .focused($focusedField, equals: FocusTarget.none)
        .onAppear {
            // Set initial selection
            if !devices.isEmpty {
                selection = [devices[0]]
            }

            // Initial focus
            focusedField = .devices
        }
    }
    
}
