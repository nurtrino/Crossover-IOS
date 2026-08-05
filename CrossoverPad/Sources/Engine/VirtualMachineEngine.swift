import Foundation

/// Abstraction over whatever actually executes a bottle's guest.
///
/// Backends (see docs/ARCHITECTURE.md § Engine layer):
/// - `MockEngine`        — dev/test fake, ships now
/// - `QEMUTCTIEngine`    — QEMU interpreter via QEMUKit (Phase 2, all channels)
/// - `QEMUJITEngine`     — JIT-enabled QEMU where the install channel allows it
protocol VirtualMachineEngine: Sendable {
    /// Human-readable backend name, surfaced in Settings.
    var displayName: String { get }

    /// True when this backend can run on the current device + install channel.
    var isAvailable: Bool { get }

    /// Boot the bottle's guest and block until it is up (or throw).
    func start(bottle: Bottle) async throws

    /// Graceful shutdown; must be safe to call on a non-running bottle.
    func stop(bottle: Bottle) async
}

/// Runtime probe for what the current install channel permits.
enum EngineCapabilities {
    /// Whether runtime code generation is available to this process.
    ///
    /// Phase 2: implement the standard probe (attempt an RWX map / check for
    /// the debugger-assisted path) rather than trusting build flags — the same
    /// binary may ship through channels with different entitlements.
    static func jitAvailable() -> Bool {
        false
    }
}
