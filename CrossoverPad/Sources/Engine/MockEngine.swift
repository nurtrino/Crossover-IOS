import Foundation

/// Fake engine for UI development and tests: "boots" after a short delay.
final class MockEngine: VirtualMachineEngine {
    let displayName = "Mock (no emulation)"
    let isAvailable = true

    private let bootDelay: Duration

    init(bootDelay: Duration = .seconds(1)) {
        self.bootDelay = bootDelay
    }

    func start(bottle: Bottle) async throws {
        try await Task.sleep(for: bootDelay)
    }

    func stop(bottle: Bottle) async {}
}
