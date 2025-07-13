//
//  Bundle+Info.swift
//  Open GPIB
//
//  Created by Thiago Mattos on 13/07/25.
//


import Foundation

extension Bundle {

    static var mainBundleIdentifier: String {
        Bundle.main.bundleIdentifier ?? "Unknown"
    }

    static var appName: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleName") as? String ?? "Unknown App"
    }

    static var displayName: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleDisplayName") as? String
        ?? Bundle.main.object(forInfoDictionaryKey: "CFBundleName") as? String
        ?? "Unknown App"
    }

    static var copyrightString: String? {
        Bundle.main.object(forInfoDictionaryKey: "NSHumanReadableCopyright") as? String
    }

    /// Returns the main bundle's version string if available (e.g. 1.0.0)
    static var versionString: String? {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String
    }

    /// Returns the main bundle's build string if available (e.g. 123)
    static var buildString: String? {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String
    }

    static var versionPostfix: String? {
        Bundle.main.object(forInfoDictionaryKey: "CE_VERSION_POSTFIX") as? String
    }
}
