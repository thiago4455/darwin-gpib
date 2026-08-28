import Foundation
import os

/// Makes `libgpib.dylib` discoverable by Python (gpib_ctypes, and through it
/// pyvisa-py) without the user configuring anything.
///
/// ## Why this is needed at all
///
/// `gpib_ctypes` loads its library with `ctypes.cdll.LoadLibrary("libgpib.so.0")`
/// — a bare leaf name, which goes to `dlopen`. On macOS that only ever tries
/// `libgpib.so.0` relative to the cwd and `/usr/lib/libgpib.so.0`; `/usr/lib` is
/// SIP-protected and unwritable even with admin, and neither `~/lib` nor
/// `/usr/local/lib` is in dyld's default fallback path any more, despite what
/// the historical documentation says. So there is no directory we can simply
/// install into. `gpib_ctypes` also never tries `libgpib.dylib` — its non-Windows
/// branch only knows the Linux names — which is why the symlink below is named
/// `libgpib.so.0` on macOS. `dlopen` reads the Mach-O header and ignores the
/// extension, so the Linux name loads a dylib perfectly well.
///
/// That leaves `DYLD_FALLBACK_LIBRARY_PATH`, which works but has to reach
/// processes we do not launch.
///
/// ## Why `launchctl setenv`
///
/// Verified on macOS 26.5: a variable set with `launchctl setenv` is inherited
/// by processes launchd spawns afterwards — GUI apps opened through Launch
/// Services, new Terminal windows, Jupyter kernels, IDE-hosted interpreters —
/// and dyld does honour a `DYLD_*` variable delivered this way (a launchd-spawned
/// Python successfully `dlopen`ed the library through it). Shell rc files were
/// rejected as the primary mechanism because they miss every GUI-launched
/// process, which is exactly the case that matters here.
///
/// Two limits worth knowing, neither fixable from here:
///   * it only affects processes started *after* the call, so an already-open
///     Terminal or a running Jupyter kernel keeps the old environment;
///   * it is scoped to the login session, so it must be re-applied at each
///     login (see `installLoginAgent()`).
///
/// This deliberately does NOT live in the dext (DriverKit sandboxes forbid
/// spawning processes or touching the login session) nor in gpibd, which starts
/// on demand when a client connects — the client being Python, which needs this
/// variable to find the library that reaches gpibd in the first place. The app
/// is the only component reliably running at the moment the user asks for it.
enum PythonSupport {

    private static let log = Logger(subsystem: "app.saturno.darwin-gpib",
                                    category: "PythonSupport")

    private static let variable = "DYLD_FALLBACK_LIBRARY_PATH"

    /// dyld's built-in fallback list. Setting the variable at all shadows this
    /// default, so when the variable was previously unset we must spell it out
    /// or we would be *replacing* the search path rather than extending it.
    private static let dyldBuiltinDefault = "/usr/local/lib:/lib:/usr/lib"

    private static let agentLabel = "app.saturno.darwin-gpib.pythonpath"

    /// `~/Library/Application Support/Open GPIB/lib`, an app-owned directory.
    static var libraryDirectory: URL? {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)
            .first?
            .appendingPathComponent("Open GPIB/lib", isDirectory: true)
    }

    /// Whether a previous install is still in place.
    ///
    /// Checked before installing so that pressing the driver button again does
    /// not re-register the login agent: that registration is what makes macOS
    /// raise its "Background Items Added" notification, and repeating it for
    /// something already set up is just noise.
    ///
    /// The symlink is checked with `fileExists`, which follows the link, so a
    /// dangling one (the app was deleted or moved) counts as not installed and
    /// will be repaired.
    static var isInstalled: Bool {
        guard let dir = libraryDirectory else { return false }
        let link = dir.appendingPathComponent("libgpib.so.0")
        return FileManager.default.fileExists(atPath: link.path)
            && FileManager.default.fileExists(atPath: loginAgentPlist().path)
            && FileManager.default.fileExists(atPath: shellSnippet().path)
    }

    /// Re-point the symlink at the running bundle, without touching the launch
    /// agent or the search path. Silent — safe to call on every activation.
    @discardableResult
    static func refreshSymlink() -> Bool {
        guard let dir = libraryDirectory, let dylib = bundledLibrary() else { return false }
        return installSymlink(in: dir, to: dylib)
    }

    /// Install the symlink, extend the search path for this login session, and
    /// install the agent that re-applies it at future logins. Safe to call
    /// repeatedly; every step is idempotent.
    @discardableResult
    static func install() -> Bool {
        guard let dir = libraryDirectory, let dylib = bundledLibrary() else {
            log.error("no bundled libgpib.dylib; skipping Python support")
            return false
        }
        guard installSymlink(in: dir, to: dylib) else { return false }
        extendSearchPath(with: dir)
        installLoginAgent(for: dir)
        installShellSnippet(for: dir)
        return true
    }

    /// Remove everything `install()` created. The search-path entry is dropped
    /// from the session too, leaving whatever the user had before untouched.
    static func uninstall() {
        guard let dir = libraryDirectory else { return }
        removeSearchPathEntry(dir)
        removeShellSnippet()
        _ = runLaunchctl(["bootout", "gui/\(getuid())/\(agentLabel)"])
        try? FileManager.default.removeItem(at: loginAgentPlist())
        try? FileManager.default.removeItem(at: dir)
    }

    // MARK: - Symlink

    private static func bundledLibrary() -> URL? {
        let url = Bundle.main.bundleURL
            .appendingPathComponent("Contents/Frameworks/libgpib.dylib")
        return FileManager.default.fileExists(atPath: url.path) ? url : nil
    }

    /// Named `libgpib.so.0` on purpose — see the type comment.
    ///
    /// A symlink rather than a copy: it cannot go stale when the app is updated,
    /// and it keeps exactly one signed copy of the library, the one in the
    /// bundle. It does dangle if the app is deleted, which is the correct
    /// outcome — the library really is gone at that point.
    private static func installSymlink(in dir: URL, to dylib: URL) -> Bool {
        let fm = FileManager.default
        let link = dir.appendingPathComponent("libgpib.so.0")
        do {
            try fm.createDirectory(at: dir, withIntermediateDirectories: true)
            // Re-point unconditionally: the bundle may have moved (a dev build
            // in DerivedData vs. /Applications) since the last run.
            if let existing = try? fm.destinationOfSymbolicLink(atPath: link.path),
               existing == dylib.path {
                return true
            }
            try? fm.removeItem(at: link)
            try fm.createSymbolicLink(at: link, withDestinationURL: dylib)
            log.info("linked \(link.path, privacy: .public) -> \(dylib.path, privacy: .public)")
            return true
        } catch {
            log.error("symlink failed: \(error.localizedDescription, privacy: .public)")
            return false
        }
    }

    // MARK: - Search path

    /// Prepend our directory, preserving anything already there.
    ///
    /// Extending rather than replacing matters in two ways: another tool may
    /// have set the variable for its own libraries, and — less obviously — an
    /// *unset* variable is not an empty one, because dyld falls back to a
    /// built-in default that setting the variable would silently shadow.
    private static func extendSearchPath(with dir: URL) {
        let current = runLaunchctl(["getenv", variable])?
            .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        var entries = current.isEmpty
            ? dyldBuiltinDefault.components(separatedBy: ":")
            : current.components(separatedBy: ":")

        guard !entries.contains(dir.path) else { return }   // already installed
        entries.insert(dir.path, at: 0)
        setSearchPath(entries.joined(separator: ":"))
    }

    private static func removeSearchPathEntry(_ dir: URL) {
        guard let current = runLaunchctl(["getenv", variable])?
                .trimmingCharacters(in: .whitespacesAndNewlines), !current.isEmpty else { return }
        let entries = current.components(separatedBy: ":").filter { $0 != dir.path }
        // If we were the only entry, unset rather than leaving the built-in
        // default frozen into an explicit value.
        if entries.isEmpty || entries.joined(separator: ":") == dyldBuiltinDefault {
            _ = runLaunchctl(["unsetenv", variable])
        } else {
            setSearchPath(entries.joined(separator: ":"))
        }
    }

    private static func setSearchPath(_ value: String) {
        _ = runLaunchctl(["setenv", variable, value])
        log.info("\(variable, privacy: .public) = \(value, privacy: .public)")
    }

    // MARK: - Persistence across logins

    private static func loginAgentPlist() -> URL {
        FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("LaunchAgents/\(agentLabel).plist")
    }

    /// `launchctl setenv` is scoped to the login session, so a reboot loses it.
    /// This agent re-applies it at login and exits immediately — a one-shot,
    /// not a resident daemon.
    ///
    /// It reads the current value and prepends, exactly as `extendSearchPath`
    /// does, so it cannot clobber a value set by something that ran earlier.
    private static func installLoginAgent(for dir: URL) {
        let script = """
        current=$(launchctl getenv \(variable))
        case ":$current:" in
          *":\(dir.path):"*) exit 0 ;;
        esac
        if [ -n "$current" ]; then
          launchctl setenv \(variable) "\(dir.path):$current"
        else
          launchctl setenv \(variable) "\(dir.path):\(dyldBuiltinDefault)"
        fi
        """
        let plist: [String: Any] = [
            "Label": agentLabel,
            "ProgramArguments": ["/bin/sh", "-c", script],
            "RunAtLoad": true,
        ]
        let url = loginAgentPlist()
        do {
            try FileManager.default.createDirectory(
                at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            let data = try PropertyListSerialization.data(
                fromPropertyList: plist, format: .xml, options: 0)
            try data.write(to: url, options: .atomic)
            // Reload so an updated script takes effect now rather than next login.
            _ = runLaunchctl(["bootout", "gui/\(getuid())/\(agentLabel)"])
            _ = runLaunchctl(["bootstrap", "gui/\(getuid())", url.path])
        } catch {
            log.error("login agent install failed: \(error.localizedDescription, privacy: .public)")
        }
    }

    // MARK: - Shell startup files

    /// Terminal shells need their own copy of this, and there is no way around
    /// it.
    ///
    /// `launchctl setenv` reaches everything launchd spawns — GUI apps, Jupyter
    /// kernels, IDE-hosted interpreters — but NOT a terminal shell. Terminal
    /// starts the shell through `/usr/bin/login`, which is setuid root, and dyld
    /// strips every `DYLD_*` variable when exec'ing a setuid binary. The value
    /// is still in the login session (`launchctl getenv` shows it); it simply
    /// cannot survive that hop. Shell rc files run after `login` has done its
    /// stripping, which is why this works where the session variable does not.
    ///
    /// So the two mechanisms are complements, not alternatives: neither covers
    /// the other's case.
    private static let beginMarker = "# >>> Open GPIB >>>"
    private static let endMarker   = "# <<< Open GPIB <<<"

    private static func shellSnippet() -> URL {
        libraryDirectory!.deletingLastPathComponent().appendingPathComponent("env.sh")
    }

    /// Shell files we add the `source` line to, when they exist — plus
    /// `.zshrc`, which is created if missing since zsh is the macOS default.
    private static func shellStartupFiles() -> [URL] {
        let home = FileManager.default.homeDirectoryForCurrentUser
        let zshrc = home.appendingPathComponent(".zshrc")
        var files = [zshrc]
        for name in [".bash_profile", ".bashrc", ".profile"] {
            let url = home.appendingPathComponent(name)
            if FileManager.default.fileExists(atPath: url.path) { files.append(url) }
        }
        return files
    }

    /// Write an app-owned `env.sh` and source it from the user's rc files,
    /// inside sentinel markers.
    ///
    /// The indirection is deliberate: the rc files get exactly one line that
    /// never has to change, and everything we might want to revise later lives
    /// in a file we own. The markers make the block findable for update and
    /// removal, so this cannot accrete duplicates across reinstalls.
    private static func installShellSnippet(for dir: URL) {
        // Same extension rule as the session variable: prepend, keep what is
        // there, and spell out dyld's built-in default when the variable was
        // unset, because setting it at all shadows that default.
        let body = """
        # Written by Open GPIB. Edits here are overwritten on reinstall.
        # Makes libgpib.dylib discoverable by gpib_ctypes / pyvisa-py.
        case ":$DYLD_FALLBACK_LIBRARY_PATH:" in
          *":\(dir.path):"*) ;;
          *)
            if [ -n "$DYLD_FALLBACK_LIBRARY_PATH" ]; then
              export DYLD_FALLBACK_LIBRARY_PATH="\(dir.path):$DYLD_FALLBACK_LIBRARY_PATH"
            else
              export DYLD_FALLBACK_LIBRARY_PATH="\(dir.path):\(dyldBuiltinDefault)"
            fi
            ;;
        esac

        """
        let snippet = shellSnippet()
        do {
            try FileManager.default.createDirectory(
                at: snippet.deletingLastPathComponent(), withIntermediateDirectories: true)
            try body.write(to: snippet, atomically: true, encoding: .utf8)
        } catch {
            log.error("env.sh write failed: \(error.localizedDescription, privacy: .public)")
            return
        }

        let block = """
        \(beginMarker)
        [ -f "\(snippet.path)" ] && . "\(snippet.path)"
        \(endMarker)
        """
        for file in shellStartupFiles() {
            let existing = (try? String(contentsOf: file, encoding: .utf8)) ?? ""
            if existing.contains(beginMarker) { continue }     // already sourced
            let separator = existing.isEmpty || existing.hasSuffix("\n") ? "" : "\n"
            let updated = existing + separator + "\n" + block + "\n"
            do {
                try updated.write(to: file, atomically: true, encoding: .utf8)
                log.info("sourced env.sh from \(file.lastPathComponent, privacy: .public)")
            } catch {
                log.error("could not update \(file.lastPathComponent, privacy: .public): \(error.localizedDescription, privacy: .public)")
            }
        }
    }

    /// Strip our sentinel block back out, leaving the rest of the file alone.
    private static func removeShellSnippet() {
        for file in shellStartupFiles() {
            guard let text = try? String(contentsOf: file, encoding: .utf8),
                  text.contains(beginMarker) else { continue }
            let lines = text.components(separatedBy: "\n")
            var kept: [String] = []
            var inBlock = false
            for line in lines {
                if line.contains(beginMarker) { inBlock = true; continue }
                if line.contains(endMarker) { inBlock = false; continue }
                if !inBlock { kept.append(line) }
            }
            try? kept.joined(separator: "\n").write(to: file, atomically: true, encoding: .utf8)
        }
        try? FileManager.default.removeItem(at: shellSnippet())
    }

    // MARK: - launchctl

    @discardableResult
    private static func runLaunchctl(_ arguments: [String]) -> String? {
        let task = Process()
        task.executableURL = URL(fileURLWithPath: "/bin/launchctl")
        task.arguments = arguments
        let pipe = Pipe()
        task.standardOutput = pipe
        task.standardError = FileHandle.nullDevice
        do {
            try task.run()
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            task.waitUntilExit()
            guard task.terminationStatus == 0 else { return nil }
            return String(data: data, encoding: .utf8)
        } catch {
            log.error("launchctl \(arguments.first ?? "", privacy: .public) failed: \(error.localizedDescription, privacy: .public)")
            return nil
        }
    }
}
