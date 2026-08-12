import Foundation
import Combine

/// Owns the bottle collection and its persistence.
///
/// Persistence is a single JSON index for metadata; per-bottle disk overlays
/// live next to it and are managed by the engine layer (Phase 2+).
@MainActor
final class BottleStore: ObservableObject {
    @Published private(set) var bottles: [Bottle] = []
    @Published var selectedBottleID: Bottle.ID?

    let engine: VirtualMachineEngine
    private let indexURL: URL

    var selectedBottle: Bottle? {
        bottles.first { $0.id == selectedBottleID }
    }

    /// Picks the real engine when this build embeds a complete Wine runtime
    /// (a runtime-bearing IPA), otherwise the mock. Either way the engine tier
    /// shown in the UI is truthful.
    nonisolated static func defaultEngine() -> VirtualMachineEngine {
        if let runtime = WineRuntimeBundle.detect(), runtime.isComplete {
            return NativeWineEngine(runtime: runtime)
        }
        return MockEngine()
    }

    init(
        engine: VirtualMachineEngine = BottleStore.defaultEngine(),
        directory: URL = .documentsDirectory.appending(path: "Bottles"),
        preview: Bool = false
    ) {
        self.engine = engine
        self.indexURL = directory.appending(path: "index.json")
        if preview {
            bottles = [
                Bottle(name: "Notepad++", state: .ready, entryPoint: "C:/Program Files/Notepad++/notepad++.exe"),
                Bottle(name: "7-Zip Installer", state: .installing),
            ]
            selectedBottleID = bottles.first?.id
        } else {
            load()
        }
    }

    // MARK: - Mutations

    func createBottle(name: String, guestType: Bottle.GuestType) -> Bottle {
        let bottle = Bottle(name: name, guestType: guestType)
        bottles.append(bottle)
        selectedBottleID = bottle.id
        save()
        return bottle
    }

    func update(_ bottle: Bottle) {
        guard let index = bottles.firstIndex(where: { $0.id == bottle.id }) else { return }
        bottles[index] = bottle
        save()
    }

    func delete(_ bottle: Bottle) {
        bottles.removeAll { $0.id == bottle.id }
        if selectedBottleID == bottle.id {
            selectedBottleID = bottles.first?.id
        }
        save()
    }

    func run(_ bottle: Bottle) async {
        var running = bottle
        running.state = .running
        running.lastError = nil
        update(running)
        do {
            try await engine.start(bottle: bottle)
        } catch {
            running.state = .broken
            // Keep the engine's specific reason — "broken" on its own is
            // useless to whoever is holding the device.
            running.lastError = (error as? LocalizedError)?.errorDescription
                ?? error.localizedDescription
            update(running)
            return
        }
        running.state = bottle.entryPoint == nil ? .created : .ready
        running.lastError = nil
        update(running)
    }

    func stop(_ bottle: Bottle) async {
        await engine.stop(bottle: bottle)
        var stopped = bottle
        stopped.state = stopped.entryPoint == nil ? .created : .ready
        update(stopped)
    }

    // MARK: - Persistence

    private func load() {
        guard let data = try? Data(contentsOf: indexURL) else { return }
        bottles = (try? JSONDecoder().decode([Bottle].self, from: data)) ?? []
    }

    private func save() {
        do {
            try FileManager.default.createDirectory(
                at: indexURL.deletingLastPathComponent(),
                withIntermediateDirectories: true
            )
            let data = try JSONEncoder().encode(bottles)
            try data.write(to: indexURL, options: .atomic)
        } catch {
            assertionFailure("Failed to persist bottle index: \(error)")
        }
    }
}
