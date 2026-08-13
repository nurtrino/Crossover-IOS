import Foundation

/// Describes the Wine runtime embedded in the app bundle, if any.
///
/// The runtime-bearing IPA pipeline copies three things into
/// `CrossoverPad.app/WineRuntime/`:
/// - `lib/` — the cross-built iOS-native Wine libraries (`ntdll.so`,
///   `win32u.so`, `wineserver`, …) as arm64 Mach-O,
/// - `pe/` — the OS-independent Windows PE DLLs,
/// - `prefix/` — the prepared Wine prefix (iOS cannot create one at runtime).
///
/// This type reports what is actually present so the UI and the engine can be
/// honest about the runtime tier instead of guessing. Presence of the files is
/// verifiable; whether they *execute* on a given device depends on the install
/// channel granting JIT (W^X) — see `EngineCapabilities.jitAvailable()`.
struct WineRuntimeBundle {
    let root: URL
    let hasLibraries: Bool
    let hasPEDLLs: Bool
    let hasPrefix: Bool
    let libraryNames: [String]

    /// True when every piece needed to attempt launching a guest is present.
    var isComplete: Bool { hasLibraries && hasPEDLLs && hasPrefix }

    /// Layout (build-tree so Wine's dladdr path-detection runs in "build_dir"
    /// mode and finds everything relative to ntdll.so):
    ///   WineRuntime/rt/dlls/ntdll/ntdll.so         → build_dir = WineRuntime/rt
    ///   WineRuntime/rt/dlls/<name>/aarch64-windows/<name>.dll
    ///   WineRuntime/rt/dlls/<name>/<name>.so
    ///   WineRuntime/rt/programs/<name>/aarch64-windows/<name>.exe
    ///   WineRuntime/rt/loader/wine, rt/server/wineserver, rt/nls/*.nls
    ///   WineRuntime/prefix/…
    static func detect(in bundle: Bundle = .main) -> WineRuntimeBundle? {
        guard let root = bundle.url(forResource: "WineRuntime", withExtension: nil) else {
            return nil
        }
        let fm = FileManager.default
        let rt = root.appendingPathComponent("rt")
        let ntdll = rt.appendingPathComponent("dlls/ntdll/ntdll.so")
        let hasLibs = fm.fileExists(atPath: ntdll.path)
        let hasPE = fm.fileExists(atPath: rt.appendingPathComponent("dlls/kernel32/aarch64-windows/kernel32.dll").path)
            || fm.fileExists(atPath: rt.appendingPathComponent("programs/cmd/aarch64-windows/cmd.exe").path)
        let hasPrefix = fm.fileExists(
            atPath: root.appendingPathComponent("prefix/drive_c/windows/system32/kernel32.dll").path
        )
        // Count the shipped unix libs for the status summary.
        let soDir = rt.appendingPathComponent("dlls")
        let names = (try? fm.contentsOfDirectory(atPath: soDir.path)) ?? []
        return WineRuntimeBundle(
            root: root,
            hasLibraries: hasLibs,
            hasPEDLLs: hasPE,
            hasPrefix: hasPrefix,
            libraryNames: names.sorted()
        )
    }

    private static func directoryHasEntries(_ fm: FileManager, _ url: URL) -> Bool {
        var isDir: ObjCBool = false
        guard fm.fileExists(atPath: url.path, isDirectory: &isDir), isDir.boolValue else { return false }
        return ((try? fm.contentsOfDirectory(atPath: url.path))?.isEmpty == false)
    }

    /// A short human-readable summary for the UI (Settings / bottle detail).
    var statusSummary: String {
        if isComplete {
            return "Embedded (\(libraryNames.count) libs)\(EngineCapabilities.jitAvailable() ? ", JIT available" : ", JIT unavailable")"
        }
        if !hasLibraries && !hasPEDLLs && !hasPrefix {
            return "Not embedded (shell build)"
        }
        var missing: [String] = []
        if !hasLibraries { missing.append("libs") }
        if !hasPEDLLs { missing.append("PE DLLs") }
        if !hasPrefix { missing.append("prefix") }
        return "Partial — missing \(missing.joined(separator: ", "))"
    }
}
