//
//  GPIBDiscovery.swift
//  Open GPIB
//
//  Enumerates GPIB adapters and the instruments hanging off them.
//
//  Adapters come from IOKit alone, so they list even when libgpib cannot be
//  reached — a missing user client should still show you that the hardware
//  attached. Instruments need the full stack.
//

import Foundation
import IOKit

// MARK: - Model

struct GPIBInstrument: Identifiable, Hashable {
    let id: String
    let boardIndex: Int
    let primaryAddress: Int
    /// Raw *IDN? response, when the instrument answered.
    let identification: String?

    /// "Keithley MODEL 2000" when *IDN? parsed, else a neutral placeholder.
    var title: String {
        guard let fields = identificationFields, fields.count >= 2 else {
            return "Instrument at \(primaryAddress)"
        }
        let maker = Self.prettifyManufacturer(fields[0])
        let model = fields[1].trimmingCharacters(in: .whitespaces)
        return model.isEmpty ? maker : "\(maker) \(model)"
    }

    var subtitle: String {
        let address = "GPIB\(boardIndex)::\(primaryAddress)"
        guard let fields = identificationFields else {
            return identification == nil
                ? "\(address) · no response to *IDN?"
                : address
        }
        // Third field is conventionally the serial number.
        if fields.count >= 3 {
            let serial = fields[2].trimmingCharacters(in: .whitespaces)
            if !serial.isEmpty && serial != "0" {
                return "\(address) · S/N \(serial)"
            }
        }
        return address
    }

    private var identificationFields: [String]? {
        guard let idn = identification, idn.contains(",") else { return nil }
        return idn.components(separatedBy: ",")
    }

    /// Instrument vendors overwhelmingly shout their name in *IDN?.
    /// "KEITHLEY INSTRUMENTS INC." reads better as "Keithley".
    private static func prettifyManufacturer(_ raw: String) -> String {
        let cleaned = raw
            .trimmingCharacters(in: .whitespaces)
            .replacingOccurrences(of: " INSTRUMENTS INC.", with: "")
            .replacingOccurrences(of: " INSTRUMENTS", with: "")
            .replacingOccurrences(of: " TECHNOLOGIES", with: "")
            .replacingOccurrences(of: ",", with: "")
        guard cleaned == cleaned.uppercased(), cleaned.count > 3 else { return cleaned }
        return cleaned.capitalized
    }
}

struct GPIBAdapter: Identifiable, Hashable {
    let id: String
    /// Friendly model name, e.g. "Keithley KUSB-488B".
    let modelName: String
    /// libgpib board index — the position among matching services, which is
    /// how libgpib's copy_service_for_board() resolves a board too.
    let boardIndex: Int
    /// Set only when more than one adapter of the same model is attached.
    var ordinal: Int?
    var instruments: [GPIBInstrument] = []
    /// The instrument scan could not run at all (no libgpib / no board).
    var scanUnavailable: Bool = false
    /// The scan ran but returned a physically impossible result, so its
    /// output is being withheld rather than shown as fact.
    var scanUnreliable: Bool = false

    var displayName: String {
        guard let ordinal else { return modelName }
        return "\(modelName) (\(ordinal))"
    }
}

// MARK: - Discovery

enum GPIBDiscovery {

    /// IOUserClass -> friendly name. Keep in sync with driver/Info.plist.
    private static let knownAdapters: [String: String] = [
        "kusb_488b":      "Keithley KUSB-488B",
        "ni_usb":         "NI GPIB-USB-HS",
        "agilent_82357":  "Agilent 82357",
    ]

    /// Walks IOKit for our driver personalities. Cheap and safe to call on
    /// the main thread.
    static func adapters() -> [GPIBAdapter] {
        var found: [GPIBAdapter] = []
        var iterator: io_iterator_t = 0
        guard let matching = IOServiceMatching("IOUserService") else { return [] }
        guard IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) == KERN_SUCCESS
        else { return [] }
        defer { IOObjectRelease(iterator) }

        var index = 0
        while true {
            let service = IOIteratorNext(iterator)
            if service == 0 { break }
            defer { IOObjectRelease(service) }

            guard let userClass = registryString(service, "IOUserClass"),
                  let friendly = knownAdapters[userClass] else { continue }

            // Prefer the adapter's own USB product string when it is present
            // and more specific than our table.
            let name = usbProductName(for: service) ?? friendly
            found.append(GPIBAdapter(id: "\(userClass)#\(index)",
                                     modelName: name,
                                     boardIndex: index))
            index += 1
        }
        return applyOrdinals(to: found)
    }

    /// Numbers adapters only where the model name is ambiguous, so the common
    /// single-adapter case stays clean.
    private static func applyOrdinals(to adapters: [GPIBAdapter]) -> [GPIBAdapter] {
        var counts: [String: Int] = [:]
        for adapter in adapters { counts[adapter.modelName, default: 0] += 1 }
        var seen: [String: Int] = [:]
        return adapters.map { adapter in
            guard counts[adapter.modelName, default: 0] > 1 else { return adapter }
            var copy = adapter
            let next = seen[adapter.modelName, default: 0] + 1
            seen[adapter.modelName] = next
            copy.ordinal = next
            return copy
        }
    }

    enum ScanState {
        case ok
        /// libgpib unreachable, or the board would not open.
        case unavailable
        /// Address sweep returned an impossible answer — see `maxDevicesOnBus`.
        case unreliable
    }

    /// IEEE-488 permits at most 15 devices on a bus. More "listeners" than
    /// that means the detection itself is wrong, not that the bench is
    /// crowded, so we withhold the result instead of inventing instruments.
    ///
    /// This currently trips on the KUSB-488B: GPIBBoard::listenerPresent
    /// decides from BusNDAC, and the adapter's bus-line register reads 0x00
    /// (see reverse/notes/02-usb-protocol.md), so every address looks
    /// occupied. Resolve that anomaly and this guard becomes inert.
    private static let maxDevicesOnBus = 15

    /// Blocking — sweeps primary addresses 1...30 for listeners and asks each
    /// one to identify itself. Call off the main thread.
    static func scanInstruments(boardIndex: Int) -> (instruments: [GPIBInstrument], state: ScanState) {
        let lib = GPIBLibrary.shared
        guard lib.isAvailable, let board = lib.findBoard(boardIndex) else {
            return ([], .unavailable)
        }

        // Address 0 is conventionally the controller; 31 is untalk/unlisten.
        let occupied = (1...30).filter {
            lib.listenerPresent(board: board, primaryAddress: $0)
        }
        guard occupied.count <= maxDevicesOnBus else {
            return ([], .unreliable)
        }

        var results: [GPIBInstrument] = []
        for pad in occupied {
            var idn: String?
            if let ud = lib.openDevice(board: boardIndex, primaryAddress: pad) {
                idn = lib.query(ud, "*IDN?\n")
                lib.goOffline(ud)
            }
            results.append(GPIBInstrument(id: "gpib\(boardIndex)::\(pad)",
                                          boardIndex: boardIndex,
                                          primaryAddress: pad,
                                          identification: idn))
        }
        // Deliberately leaving the board online: taking it offline here would
        // drop REN and force a re-init on the next refresh.
        return (results, .ok)
    }

    // MARK: - Registry helpers

    private static func registryString(_ service: io_service_t, _ key: String) -> String? {
        guard let ref = IORegistryEntryCreateCFProperty(
            service, key as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue()
        else { return nil }
        return ref as? String
    }

    /// Climbs to the owning IOUSBHostDevice for its advertised product name.
    private static func usbProductName(for service: io_service_t) -> String? {
        var current = service
        var owned = false
        for _ in 0..<6 {
            if let product = registryString(current, "USB Product Name") {
                let vendor = registryString(current, "USB Vendor Name")
                if owned { IOObjectRelease(current) }
                guard let vendor, !vendor.isEmpty else { return product }
                return "\(vendor.capitalized) \(product)"
            }
            var parent: io_registry_entry_t = 0
            let kr = IORegistryEntryGetParentEntry(current, kIOServicePlane, &parent)
            if owned { IOObjectRelease(current) }
            guard kr == KERN_SUCCESS, parent != 0 else { return nil }
            current = parent
            owned = true
        }
        if owned { IOObjectRelease(current) }
        return nil
    }
}
