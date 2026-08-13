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
            if runtimeReady {
                Section {
                    NavigationLink {
                        ConsoleView(bottle: bottle)
                    } label: {
                        Label("Open CMD Console", systemImage: "terminal")
                    }
                    NavigationLink {
                        GuestScreenView(bottle: bottle)
                    } label: {
                        Label("Launch Notepad (GUI)", systemImage: "macwindow")
                    }
                } footer: {
                    Text("Both run in-process against the embedded Wine runtime. The GUI screen presents Windows windows through the wineios.drv display driver; touch acts as the mouse. On device this needs JIT — attach StikDebug.")
                }
            } else {
                Section {
                    Button("Attempt Launch (diagnostic)") {
                        Task { await store.run(bottle) }
                    }
                    .disabled(bottle.state == .installing)
                } footer: {
                    Text("No Wine runtime is embedded in this build, so there is nothing to run. Install a runtime-bearing IPA.")
                }
            }
        }
        .navigationTitle(bottle.name)
    }

    /// The console is offered whenever a complete Wine runtime is embedded.
    private var runtimeReady: Bool {
        WineRuntimeBundle.detect()?.isComplete ?? false
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
