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

    static func detect(in bundle: Bundle = .main) -> WineRuntimeBundle? {
        guard let root = bundle.url(forResource: "WineRuntime", withExtension: nil) else {
            return nil
        }
        let fm = FileManager.default
        let lib = root.appendingPathComponent("lib")
        let names = (try? fm.contentsOfDirectory(atPath: lib.path)) ?? []
        let hasLibs = names.contains("ntdll.so") && names.contains("wineserver")
        let hasPE = directoryHasEntries(fm, root.appendingPathComponent("pe"))
        let hasPrefix = fm.fileExists(
            atPath: root.appendingPathComponent("prefix/drive_c/windows/system32/kernel32.dll").path
        )
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
