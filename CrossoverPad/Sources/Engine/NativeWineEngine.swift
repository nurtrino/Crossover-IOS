import Foundation

/// The real engine: in-process Wine on the pseudo-process substrate
/// (see docs/NATIVE_PORT.md). Lands in Phase 3 once the host-first fork
/// reaches M1; until then it reports unavailable so the UI can show the
/// engine tier honestly.
final class NativeWineEngine: VirtualMachineEngine {
    let displayName = "Native Wine (in-process)"

    /// Phase 3: probe for the bundled winelib dylibs + JIT availability
    /// (EngineCapabilities.jitAvailable()) instead of hardcoding.
    let isAvailable = false

    enum EngineError: Error {
        case notYetImplemented
    }

    func start(bottle: Bottle) async throws {
        throw EngineError.notYetImplemented
    }

    func stop(bottle: Bottle) async {}
}
