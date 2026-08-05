import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var store: BottleStore

    var body: some View {
        NavigationSplitView {
            BottleListView()
        } detail: {
            if let bottle = store.selectedBottle {
                BottleDetailView(bottle: bottle)
            } else {
                ContentUnavailableView(
                    "No Bottle Selected",
                    systemImage: "cube.transparent",
                    description: Text("Create a bottle to install and run a Windows application.")
                )
            }
        }
    }
}

#Preview {
    ContentView()
        .environmentObject(BottleStore(preview: true))
}
