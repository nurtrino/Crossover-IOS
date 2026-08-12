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
            if let reason = bottle.lastError {
                Section("Why it stopped") {
                    Text(reason)
                        .font(.callout)
                        .foregroundStyle(.secondary)
                }
            }
            Section("Device diagnostics") {
                LabeledContent("JIT (W^X)", value: jitLabel)
                if let rt = WineRuntimeBundle.detect() {
                    LabeledContent("Runtime libs", value: rt.hasLibraries ? "present (\(rt.libraryNames.count))" : "missing")
                    LabeledContent("Windows DLLs", value: rt.hasPEDLLs ? "present" : "missing")
                    LabeledContent("Prefix", value: rt.hasPrefix ? "present" : "missing")
                } else {
                    LabeledContent("Runtime", value: "not embedded")
                }
            }
            Section {
                if bottle.state == .running {
                    Button("Stop", role: .destructive) {
                        Task { await store.stop(bottle) }
                    }
                } else {
                    // Honest label: there is no installer picker yet, and the
                    // engine cannot launch a guest — this probes the runtime
                    // and reports the blocker.
                    Button("Attempt Launch (diagnostic)") {
                        Task { await store.run(bottle) }
                    }
                    .disabled(bottle.state == .installing)
                }
            } footer: {
                Text("Guest execution is not working yet. Attempting a run reports the exact blocker above — that is what this build is for. See docs/HANDOFF-M2.md.")
            }
        }
        .navigationTitle(bottle.name)
    }

    /// Probed live, so attaching a JIT-capable debugger (e.g. StikDebug) after
    /// launch flips this without reinstalling.
    private var jitLabel: String {
        EngineCapabilities.jitAvailable() ? "available" : "unavailable"
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
