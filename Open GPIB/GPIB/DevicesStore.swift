//
//  DevicesStore.swift
//  Open GPIB
//
//  Backing store for the devices panel.
//
//  Adapters are listed first and published immediately, because that part is
//  a cheap IOKit walk and is the thing you most want to see. The instrument
//  scan talks to real hardware over USB and can take a moment, so it runs off
//  the main thread and fills in afterwards.
//

import Foundation
import SwiftUI

@MainActor
final class DevicesStore: ObservableObject {

    @Published private(set) var adapters: [GPIBAdapter] = []
    @Published private(set) var isRefreshing = false
    /// Collapsed adapters, keyed by adapter id. Expanded is the default.
    @Published var collapsed: Set<String> = []

    private var refreshTask: Task<Void, Never>?

    var isEmpty: Bool { adapters.isEmpty }

    func isCollapsed(_ adapter: GPIBAdapter) -> Bool { collapsed.contains(adapter.id) }

    func toggle(_ adapter: GPIBAdapter) {
        if collapsed.contains(adapter.id) {
            collapsed.remove(adapter.id)
        } else {
            collapsed.insert(adapter.id)
        }
    }

    func refresh() {
        // A second refresh supersedes one already running rather than queueing.
        refreshTask?.cancel()
        isRefreshing = true

        let discovered = GPIBDiscovery.adapters()
        adapters = discovered

        guard !discovered.isEmpty else {
            isRefreshing = false
            return
        }

        refreshTask = Task { [weak self] in
            var scanned = discovered
            for (offset, adapter) in discovered.enumerated() {
                if Task.isCancelled { return }
                let boardIndex = adapter.boardIndex
                let result = await Task.detached(priority: .userInitiated) {
                    GPIBDiscovery.scanInstruments(boardIndex: boardIndex)
                }.value
                if Task.isCancelled { return }
                scanned[offset].instruments = result.instruments
                scanned[offset].scanUnavailable = result.state == .unavailable
                scanned[offset].scanUnreliable  = result.state == .unreliable
                // Publish incrementally so a slow second adapter does not hide
                // results from the first.
                self?.adapters = scanned
            }
            self?.isRefreshing = false
        }
    }
}
