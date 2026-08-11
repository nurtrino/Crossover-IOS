import SwiftUI

struct BottleDetailView: View {
    @EnvironmentObject private var store: BottleStore
    let bottle: Bottle

    var body: some View {
        Form {
            Section("Application") {
                LabeledContent("Name", value: bottle.name)
                LabeledContent("Status", value: bottle.state.rawValue.capitalized)
                if let entryPoint = bottle.entryPoint {
                    LabeledContent("Program", value: entryPoint)
                }
            }
            Section("Environment") {
                LabeledContent("Guest", value: guestLabel)
                LabeledContent("Engine", value: store.engine.displayName)
                if let native = store.engine as? NativeWineEngine {
                    LabeledContent("Runtime", value: native.runtimeStatus)
                }
                LabeledContent("Created", value: bottle.createdAt.formatted(date: .abbreviated, time: .shortened))
            }
            Section {
                if bottle.state == .running {
                    Button("Stop", role: .destructive) {
                        Task { await store.stop(bottle) }
                    }
                } else {
                    Button(bottle.entryPoint == nil ? "Run Installer…" : "Run") {
                        Task { await store.run(bottle) }
                    }
                    .disabled(bottle.state == .installing)
                }
            }
        }
        .navigationTitle(bottle.name)
    }

    private var guestLabel: String {
        switch bottle.guestType {
        case .linuxWine: "Linux + Wine (built-in)"
        case .windowsArm: "Windows ARM (user-supplied)"
        }
    }
}

#Preview {
    NavigationStack {
        BottleDetailView(bottle: Bottle(name: "Notepad++", state: .ready))
    }
    .environmentObject(BottleStore(preview: true))
}
