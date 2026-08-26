//
//  GPIBLibrary.swift
//  Open GPIB
//
//  Thin dlopen wrapper around the embedded libgpib.dylib.
//
//  We bind at runtime rather than linking, so the app needs no bridging
//  header and no changes to the target's build settings. The dylib ships in
//  the app bundle's Frameworks directory.
//
//  Opening the driver's user client requires the *calling process* to hold
//  com.apple.developer.driverkit.userclient-access (see
//  Open GPIB/darwin_gpib.entitlements). Without it every call here fails with
//  EDVR no matter how healthy the driver is.
//

import Foundation

final class GPIBLibrary {

    static let shared = GPIBLibrary()

    /// nil when the dylib could not be found or loaded — the UI still lists
    /// adapters in that case, it just cannot enumerate instruments.
    private let handle: UnsafeMutableRawPointer?

    // ib* entry points we actually use.
    private typealias FnFind    = @convention(c) (UnsafePointer<CChar>) -> Int32
    private typealias FnOnl     = @convention(c) (Int32, Int32) -> Int32
    private typealias FnLn      = @convention(c) (Int32, Int32, Int32, UnsafeMutablePointer<Int16>) -> Int32
    private typealias FnDev     = @convention(c) (Int32, Int32, Int32, Int32, Int32, Int32) -> Int32
    private typealias FnWrt     = @convention(c) (Int32, UnsafeRawPointer, Int) -> Int32
    private typealias FnRd      = @convention(c) (Int32, UnsafeMutableRawPointer, Int) -> Int32
    private typealias FnErrStr  = @convention(c) (Int32) -> UnsafePointer<CChar>?

    private let fnFind: FnFind?
    private let fnOnl: FnOnl?
    private let fnLn: FnLn?
    private let fnDev: FnDev?
    private let fnWrt: FnWrt?
    private let fnRd: FnRd?
    private let fnErrStr: FnErrStr?

    private let pIbsta: UnsafeMutablePointer<Int32>?
    private let pIberr: UnsafeMutablePointer<Int32>?

    var isAvailable: Bool { handle != nil && fnFind != nil }

    private init() {
        // Bind through a local rather than the stored property: the helper
        // must not touch self before every stored property is initialised.
        let loaded = GPIBLibrary.load()
        handle = loaded
        func sym<T>(_ name: String, _ type: T.Type) -> T? {
            guard let loaded, let s = dlsym(loaded, name) else { return nil }
            return unsafeBitCast(s, to: type)
        }
        fnFind   = sym("ibfind", FnFind.self)
        fnOnl    = sym("ibonl", FnOnl.self)
        fnLn     = sym("ibln", FnLn.self)
        fnDev    = sym("ibdev", FnDev.self)
        fnWrt    = sym("ibwrt", FnWrt.self)
        fnRd     = sym("ibrd", FnRd.self)
        fnErrStr = sym("gpib_error_string", FnErrStr.self)
        pIbsta = loaded.flatMap { dlsym($0, "ibsta") }?.assumingMemoryBound(to: Int32.self)
        pIberr = loaded.flatMap { dlsym($0, "iberr") }?.assumingMemoryBound(to: Int32.self)
    }

    private static func load() -> UnsafeMutableRawPointer? {
        var candidates: [String] = []
        // Explicit override, mainly so the discovery layer can be exercised
        // outside the app bundle (see tools/).
        if let override = ProcessInfo.processInfo.environment["GPIB_LIBRARY_PATH"] {
            candidates.append(override)
        }
        if let frameworks = Bundle.main.privateFrameworksURL {
            candidates.append(frameworks.appendingPathComponent("libgpib.dylib").path)
        }
        // Running from Xcode the dylib also sits beside the app in Products.
        if let exe = Bundle.main.executableURL?
            .deletingLastPathComponent()      // MacOS
            .deletingLastPathComponent()      // Contents
            .deletingLastPathComponent()      // .app
            .deletingLastPathComponent() {    // Products/Debug
            candidates.append(exe.appendingPathComponent("libgpib.dylib").path)
        }
        candidates.append("libgpib.dylib")    // fall back to the loader's search path
        for path in candidates {
            if let h = dlopen(path, RTLD_NOW) { return h }
        }
        return nil
    }

    // MARK: - Status

    var ibsta: Int32 { pIbsta?.pointee ?? 0 }
    var iberr: Int32 { pIberr?.pointee ?? 0 }
    var lastErrorText: String {
        guard let f = fnErrStr, let s = f(iberr) else { return "error \(iberr)" }
        return String(cString: s)
    }
    /// ERR bit in ibsta.
    var lastCallFailed: Bool { (ibsta & 0x8000) != 0 }

    // MARK: - Calls

    /// Opens a board by its libgpib name, e.g. board 0 -> "gpib0".
    func findBoard(_ index: Int) -> Int32? {
        guard let f = fnFind else { return nil }
        let ud = "gpib\(index)".withCString { f($0) }
        return ud >= 0 ? ud : nil
    }

    func goOffline(_ ud: Int32) {
        _ = fnOnl?(ud, 0)
    }

    /// True when a listener responds at this primary address.
    func listenerPresent(board: Int32, primaryAddress: Int) -> Bool {
        guard let f = fnLn else { return false }
        var present: Int16 = 0
        _ = f(board, Int32(primaryAddress), 0, &present)
        if lastCallFailed { return false }
        return present != 0
    }

    /// Opens a device handle. timeoutCode 13 == T3s in linux-gpib's table.
    func openDevice(board: Int, primaryAddress: Int, timeoutCode: Int32 = 13) -> Int32? {
        guard let f = fnDev else { return nil }
        let ud = f(Int32(board), Int32(primaryAddress), 0, timeoutCode, 1, 0)
        return ud >= 0 ? ud : nil
    }

    /// Writes a command and reads the reply. Returns nil if either half fails.
    func query(_ ud: Int32, _ command: String, readLimit: Int = 256) -> String? {
        guard let w = fnWrt, let r = fnRd else { return nil }
        var bytes = Array(command.utf8)
        let written = bytes.withUnsafeBytes { raw -> Int32 in
            guard let base = raw.baseAddress else { return -1 }
            return w(ud, base, bytes.count)
        }
        if written < 0 || lastCallFailed { return nil }

        var buffer = [UInt8](repeating: 0, count: readLimit)
        let read = buffer.withUnsafeMutableBytes { raw -> Int32 in
            guard let base = raw.baseAddress else { return -1 }
            return r(ud, base, readLimit - 1)
        }
        if read < 0 || lastCallFailed { return nil }

        // libgpib reports the transferred count in ibcnt; the buffer is
        // pre-zeroed so trimming at the first NUL is equivalent and avoids
        // depending on another global.
        let text = String(decoding: buffer.prefix(while: { $0 != 0 }), as: UTF8.self)
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? nil : trimmed
    }
}
