import SwiftUI

struct BottleListView: View {
    @EnvironmentObject private var store: BottleStore
    @State private var showingNewBottle = false

    var body: some View {
        List(selection: $store.selectedBottleID) {
            ForEach(store.bottles) { bottle in
                BottleRow(bottle: bottle)
                    .tag(bottle.id)
            }
            .onDelete { offsets in
                for offset in offsets {
                    store.delete(store.bottles[offset])
                }
            }
        }
        .overlay {
            if store.bottles.isEmpty {
                ContentUnavailableView(
                    "No Bottles",
                    systemImage: "plus.circle",
                    description: Text("Each bottle holds one Windows application.")
                )
            }
        }
        .navigationTitle("Bottles")
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Button {
                    showingNewBottle = true
                } label: {
                    Label("New Bottle", systemImage: "plus")
                }
            }
        }
        .sheet(isPresented: $showingNewBottle) {
            NewBottleWizard()
        }
    }
}

private struct BottleRow: View {
    let bottle: Bottle

    var body: some View {
        HStack {
            Image(systemName: icon)
                .foregroundStyle(color)
            VStack(alignment: .leading) {
                Text(bottle.name)
                Text(bottle.state.rawValue.capitalized)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
    }

    private var icon: String {
        switch bottle.state {
        case .running: "play.circle.fill"
        case .broken: "exclamationmark.triangle.fill"
        case .installing: "arrow.down.circle"
        default: "cube"
        }
    }

    private var color: Color {
        switch bottle.state {
        case .running: .green
        case .broken: .red
        default: .secondary
        }
    }
}

#Preview {
    NavigationStack {
        BottleListView()
    }
    .environmentObject(BottleStore(preview: true))
}
