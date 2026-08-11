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
    /// Whether runtime code generation is available to this process *right now*.
    ///
    /// This is a live probe, not a build flag — the same binary ships through
    /// channels with different capabilities. Two signals, in order:
    ///
    /// 1. `CS_DEBUGGED`: a debugger-assisted channel (e.g. StikDebug, or any
    ///    `debugserver`/lldb attach) puts the process in the debugged state,
    ///    which grants the `dynamic-codesigning` path the PE loader needs.
    ///    This is the signal that flips once the debugger has attached, so it
    ///    is re-evaluated on each call rather than cached.
    /// 2. As a fallback, actually attempt a writable+executable mapping — the
    ///    ground truth for "can this process generate and run code."
    static func jitAvailable() -> Bool {
        if let flags = codeSignStatus(), flags & CS_DEBUGGED != 0 { return true }
        return canMapWritableExecutable()
    }

    // csops(2) status bits (xnu <sys/codesign.h>)
    private static let CS_OPS_STATUS: UInt32 = 0
    private static let CS_DEBUGGED: UInt32 = 0x1000_0000

    /// Read this process's code-signing status word via `csops`.
    private static func codeSignStatus() -> UInt32? {
        typealias CsopsFn = @convention(c)
            (Int32, UInt32, UnsafeMutableRawPointer?, Int) -> Int32
        // RTLD_DEFAULT (-2) — csops is exported from libsystem_kernel.
        guard let sym = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "csops") else {
            return nil
        }
        let csops = unsafeBitCast(sym, to: CsopsFn.self)
        var status: UInt32 = 0
        let rc = withUnsafeMutableBytes(of: &status) {
            csops(getpid(), CS_OPS_STATUS, $0.baseAddress, $0.count)
        }
        return rc == 0 ? status : nil
    }

    /// Ground-truth probe: can we map a page writable+executable? Without a
    /// JIT-granting channel iOS refuses this and the loader has no path to run
    /// guest code.
    private static func canMapWritableExecutable() -> Bool {
        let size = Int(getpagesize())
        let ptr = mmap(nil, size,
                       PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)
        guard ptr != MAP_FAILED else { return false }
        munmap(ptr, size)
        return true
    }
}
