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

    @StateObject private var store = DevicesStore()
    /// Instrument ids — see GPIBInstrument.id.
    @State private var selection: Set<String> = []

    public init() {}

    public var body: some View {
        let dismiss = dismissWindow.callAsFunction

        return HStack(spacing: 0) {
            WelcomeView(
                dismissWindow: dismiss,
                focusedField: $focusedField
            )

            Group {
                DevicesListView(
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
        .environmentObject(store)
        .cursor(.current)
        .edgesIgnoringSafeArea(.top)
        .focused($focusedField, equals: FocusTarget.none)
        .onAppear {
            // The broker must exist before any GPIB call; registering is
            // idempotent so this is safe on every launch.
            AgentManager.shared.registerIfNeeded()

            // The scan this triggers sweeps every GPIB address (1...30) with
            // real bus traffic — addressing, a bus-line read, and a full
            // *IDN? query for anything that looks occupied. It shares the
            // same gpibd broker as every other client, and gpibd only
            // serializes one *operation* at a time, not a whole write+read
            // *transaction* — so this scan and an unrelated manual GPIB
            // session running concurrently (e.g. a CLI tool used while
            // iterating on the driver) can genuinely interleave their
            // transactions and confuse an instrument mid-query. Confirmed
            // as a real, not theoretical, source of exactly that kind of
            // confusion during driver development. Set
            // OPENGPIB_NO_AUTOSCAN=1 to launch the app (e.g. to keep gpibd
            // registered, or to use "Reconnect all adaptors") without this
            // firing, when something else is about to talk to the bus.
            if ProcessInfo.processInfo.environment["OPENGPIB_NO_AUTOSCAN"] != "1" {
                store.refresh()
            }
            focusedField = .devices
        }
    }
}
