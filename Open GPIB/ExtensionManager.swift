//
//  ExtensionManager.swift
//  darwin-gpib
//
//  Created by Thiago Mattos on 14/07/25.
//


import Foundation
import Security
import SystemExtensions
import os.log

/// What the driver button will actually do if pressed, so it can say so.
///
/// "Reconnect all adaptors" described a side effect rather than the action:
/// the button submits a system-extension activation request. Whether that
/// installs, upgrades or reinstalls depends on what is already on the system,
/// and the user had no way to tell those apart from the button alone.
enum DriverState: Equatable {
    /// Nothing installed for our identifier.
    case notInstalled
    /// Something is installed, but not the build inside this app bundle.
    case needsUpdate
    /// The installed extension is identical to the one we ship.
    case upToDate
    /// Not yet determined — the properties request is asynchronous.
    case unknown

    var buttonTitle: String {
        switch self {
        case .notInstalled: return "Install driver"
        case .needsUpdate:  return "Update driver extension"
        case .upToDate:     return "Reinstall driver"
        case .unknown:      return "Install driver"
        }
    }
}

final class ExtensionManager: NSObject, ObservableObject, OSSystemExtensionRequestDelegate {

    static let shared = ExtensionManager()

    private static let identifier = "app.saturno.darwin-gpib.driver"

    @Published private(set) var state: DriverState = .unknown

    /// Which in-flight requests are properties requests: a properties request
    /// and an activation request share this one delegate.
    private var propertiesRequests = Set<ObjectIdentifier>()

    // MARK: - State

    /// Ask the system what is installed, then compare it to what we ship.
    ///
    /// Comparison is by **CDHash**, not bundle version: during development the
    /// version string does not change between builds, so a version check would
    /// report "up to date" for a dext that is actually stale. The CDHash is the
    /// same identity `codesign -dvvv` prints, which is what has been used all
    /// along to confirm by hand that the loaded dext is the built one.
    func refreshState() {
        let request = OSSystemExtensionRequest.propertiesRequest(
            forExtensionWithIdentifier: Self.identifier, queue: .main)
        request.delegate = self
        propertiesRequests.insert(ObjectIdentifier(request))
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    /// The dext we ship inside this app bundle.
    private var bundledExtensionURL: URL {
        Bundle.main.bundleURL
            .appendingPathComponent("Contents/Library/SystemExtensions/\(Self.identifier).dext")
    }

    /// `kSecCodeInfoUnique` is the CDHash — the value `codesign -dvvv` reports.
    /// Returns nil for an unsigned or unreadable bundle, which is treated as
    /// "cannot prove it matches" rather than as a match.
    private func codeDirectoryHash(of url: URL) -> Data? {
        var staticCode: SecStaticCode?
        guard SecStaticCodeCreateWithPath(url as CFURL, [], &staticCode) == errSecSuccess,
              let staticCode else { return nil }
        var information: CFDictionary?
        guard SecCodeCopySigningInformation(staticCode, [], &information) == errSecSuccess,
              let dictionary = information as? [String: Any] else { return nil }
        return dictionary[kSecCodeInfoUnique as String] as? Data
    }

    // MARK: - Actions

    /// Submit the activation request. Installs, upgrades or reinstalls
    /// according to what is already there — the button title says which.
    func activate() {
        // Python support is installed here, on an explicit press, rather than at
        // app launch. Registering the login agent makes macOS show its
        // "Background Items Added" notification, and that notification only
        // makes sense to someone who has just asked for the driver to be
        // installed. Only install when it is actually missing, so reinstalling
        // the dext does not re-raise the notification for something already
        // set up.
        if !PythonSupport.isInstalled {
            PythonSupport.install()
        } else {
            // Cheap and silent: repoints the symlink if the app moved between a
            // DerivedData build and /Applications. Touches no launch agent, so
            // it cannot raise the notification.
            PythonSupport.refreshSymlink()
        }

        os_log("sysex activation request for %@", Self.identifier)
        let activationRequest = OSSystemExtensionRequest.activationRequest(
            forExtensionWithIdentifier: Self.identifier, queue: .main)
        activationRequest.delegate = self
        OSSystemExtensionManager.shared.submitRequest(activationRequest)
    }

    func deactivate() {
        // This doesn't seem to work in b1 not sure why
        let activationRequest = OSSystemExtensionRequest.deactivationRequest(
            forExtensionWithIdentifier: Self.identifier, queue: .main)
        activationRequest.delegate = self
        OSSystemExtensionManager.shared.submitRequest(activationRequest)
    }

    // MARK: - OSSystemExtensionRequestDelegate

    func request(_ request: OSSystemExtensionRequest,
                 foundProperties properties: [OSSystemExtensionProperties]) {
        propertiesRequests.remove(ObjectIdentifier(request))

        guard !properties.isEmpty else {
            state = .notInstalled
            os_log("sysex properties: none installed")
            return
        }

        // More than one entry shows up mid-upgrade (an old copy terminating
        // while a new one waits). Any exact match means our build is present.
        let ours = codeDirectoryHash(of: bundledExtensionURL)
        let matches = ours != nil && properties.contains { property in
            codeDirectoryHash(of: property.url) == ours
        }
        state = matches ? .upToDate : .needsUpdate
        os_log("sysex properties: %d installed", properties.count)
    }

    func request(_ request: OSSystemExtensionRequest,
                 actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        os_log("sysex actionForReplacingExtension %@ %@", existing, ext)
        return .replace
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        os_log("sysex needsUserApproval")
    }

    func request(_ request: OSSystemExtensionRequest,
                 didFinishWithResult result: OSSystemExtensionRequest.Result) {
        os_log("sysex didFinishWithResult %ld", result.rawValue)
        // A properties request that produced no foundProperties callback means
        // nothing is installed for our identifier.
        if propertiesRequests.remove(ObjectIdentifier(request)) != nil {
            state = .notInstalled
            return
        }
        // An activation finished: what is installed has just changed.
        refreshState()
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        os_log("sysex didFailWithError %@", error.localizedDescription)
        if propertiesRequests.remove(ObjectIdentifier(request)) != nil {
            state = .notInstalled
        }
    }
}
