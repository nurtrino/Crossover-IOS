import Foundation

/// One Windows program plus its isolated guest environment.
/// See docs/ARCHITECTURE.md § Bottles for the on-disk layout.
struct Bottle: Identifiable, Codable, Equatable {
    enum GuestType: String, Codable, CaseIterable {
        /// Default: Alpine ARM64 + Wine (ARM64EC) + Box64. Freely shippable.
        case linuxWine
        /// Power-user option: user-supplied Windows ARM image.
        case windowsArm
    }

    enum State: String, Codable {
        case created        // bottle exists, nothing installed yet
        case installing     // installer .exe is running in the guest
        case ready          // app installed, snapshot taken
        case running        // guest is live
        case broken         // guest failed to boot or install
    }

    let id: UUID
    var name: String
    var guestType: GuestType
    var state: State
    /// Path inside the guest to the installed program, once known.
    var entryPoint: String?
    var createdAt: Date

    init(
        id: UUID = UUID(),
        name: String,
        guestType: GuestType = .linuxWine,
        state: State = .created,
        entryPoint: String? = nil,
        createdAt: Date = .now
    ) {
        self.id = id
        self.name = name
        self.guestType = guestType
        self.state = state
        self.entryPoint = entryPoint
        self.createdAt = createdAt
    }
}
