//
//  AgentManager.swift
//  Open GPIB
//
//  Registers the gpibd broker as a login-session agent.
//
//  gpibd is the only component that holds
//  com.apple.developer.driverkit.userclient-access, and therefore the only one
//  that can open the driver. This app talks to it exactly the way a Python
//  script does — registering it is the only special relationship they have.
//
//  Registration is a per-user launchd job, so no admin rights are involved.
//

import Foundation
import ServiceManagement
import os.log

@MainActor
final class AgentManager: ObservableObject {

    static let shared = AgentManager()

    /// Must match the plist embedded at Contents/Library/LaunchAgents/.
    private static let plistName = "app.saturno.darwin-gpib.gpibd.plist"

    @Published private(set) var status: SMAppService.Status = .notRegistered
    @Published private(set) var lastError: String?

    private let service = SMAppService.agent(plistName: AgentManager.plistName)
    private let log = Logger(subsystem: "app.saturno.darwin-gpib", category: "agent")

    private init() {
        status = service.status
    }

    /// Idempotent: registering an already-enabled agent is a no-op, so this is
    /// safe to call on every launch.
    func registerIfNeeded() {
        status = service.status
        // Logged before the early return: an "already enabled" exit is the
        // common case, and silence there is indistinguishable from the call
        // never happening.
        log.info("gpibd agent status: \(self.statusText, privacy: .public)")
        guard status != .enabled else { return }
        do {
            try service.register()
            lastError = nil
            log.info("gpibd agent registered")
        } catch {
            // Not fatal. The broker may already be registered by hand during
            // development (gpibd/install.sh), in which case everything works
            // and only this bookkeeping call failed.
            lastError = error.localizedDescription
            log.error("gpibd agent register failed: \(error.localizedDescription, privacy: .public)")
        }
        status = service.status
        log.info("gpibd agent status after register: \(self.statusText, privacy: .public)")
    }

    func unregister() {
        do {
            try service.unregister()
            lastError = nil
        } catch {
            lastError = error.localizedDescription
        }
        status = service.status
    }

    /// Human-readable state for the UI.
    var statusText: String {
        switch status {
        case .notRegistered:    return "not registered"
        case .enabled:          return "enabled"
        case .requiresApproval: return "awaiting approval in Login Items"
        case .notFound:         return "not found in the app bundle"
        @unknown default:       return "unknown"
        }
    }
}
