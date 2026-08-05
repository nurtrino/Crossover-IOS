import SwiftUI

@main
struct CrossoverPadApp: App {
    @StateObject private var store = BottleStore()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(store)
        }
    }
}
