import Foundation

/// The real engine: in-process Wine on the pseudo-process substrate
/// (see docs/NATIVE_PORT.md and docs/HANDOFF-M2.md).
///
/// State of the port (honest, not aspirational):
/// - The iOS-native Wine runtime cross-compiles and links as arm64 Mach-O
///   (`ntdll.so`, `win32u.so`, `wineserver`); the runtime-bearing IPA embeds
///   it plus the PE DLLs and a prepared prefix under `WineRuntime/`.
/// - `isAvailable` reflects what is genuinely present *and* runnable: the
///   embedded runtime must be complete and the install channel must grant JIT
///   (the PE loader needs W^X). Neither is assumed — both are probed.
/// - Launching a guest still needs the on-device pieces tracked in
///   docs/HANDOFF-M2.md (the UIKit/Metal display driver for GUI apps, and the
///   loader/JIT integration). Until those land, `start` throws a specific,
///   honest error rather than pretending to run.
final class NativeWineEngine: VirtualMachineEngine {
    let displayName = "Native Wine (in-process)"

    private let runtime: WineRuntimeBundle?

    init(runtime: WineRuntimeBundle? = WineRuntimeBundle.detect()) {
        self.runtime = runtime
    }

    /// Available only when the full runtime is embedded and JIT is granted.
    /// Even then, `start` gates on the display driver / loader work — this
    /// flag means "the runtime bits are here and code can run", not "GUI works".
    var isAvailable: Bool {
        (runtime?.isComplete ?? false) && EngineCapabilities.jitAvailable()
    }

    /// One-line description of the embedded runtime for the UI.
    var runtimeStatus: String {
        runtime?.statusSummary ?? "Not embedded (shell build)"
    }

    enum EngineError: LocalizedError {
        case runtimeNotEmbedded
        case jitUnavailable
        case displayDriverMissing

        var errorDescription: String? {
            switch self {
            case .runtimeNotEmbedded:
                return "The Wine runtime is not embedded in this build (shell build). Install a runtime-bearing IPA."
            case .jitUnavailable:
                return "This install channel does not grant JIT (W^X); the PE loader cannot execute guest code. Sideload via a JIT-capable channel."
            case .displayDriverMissing:
                return "The iOS display driver is not yet implemented, so GUI programs cannot present a window. See docs/HANDOFF-M2.md."
            }
        }
    }

    func start(bottle: Bottle) async throws {
        guard let runtime, runtime.isComplete else { throw EngineError.runtimeNotEmbedded }
        guard EngineCapabilities.jitAvailable() else { throw EngineError.jitUnavailable }
        // The runtime is present and code could run, but presenting a window
        // needs the UIKit/Metal driver (M2). Fail honestly instead of
        // launching into a headless void.
        _ = runtime
        throw EngineError.displayDriverMissing
    }

    func stop(bottle: Bottle) async {}
}
