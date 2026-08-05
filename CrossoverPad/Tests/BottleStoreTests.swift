import XCTest
@testable import CrossoverPad

@MainActor
final class BottleStoreTests: XCTestCase {
    private var store: BottleStore!
    private var directory: URL!

    override func setUp() async throws {
        directory = FileManager.default.temporaryDirectory
            .appending(path: "BottleStoreTests-\(UUID().uuidString)")
        store = BottleStore(engine: MockEngine(bootDelay: .zero), directory: directory)
    }

    override func tearDown() async throws {
        try? FileManager.default.removeItem(at: directory)
    }

    func testCreateSelectsAndPersists() throws {
        let bottle = store.createBottle(name: "Test", guestType: .linuxWine)
        XCTAssertEqual(store.selectedBottleID, bottle.id)

        let reloaded = BottleStore(engine: MockEngine(bootDelay: .zero), directory: directory)
        XCTAssertEqual(reloaded.bottles.map(\.id), [bottle.id])
    }

    func testDeleteClearsSelection() {
        let bottle = store.createBottle(name: "Test", guestType: .linuxWine)
        store.delete(bottle)
        XCTAssertTrue(store.bottles.isEmpty)
        XCTAssertNil(store.selectedBottleID)
    }

    func testRunTransitionsThroughRunning() async {
        let bottle = store.createBottle(name: "Test", guestType: .linuxWine)
        await store.run(bottle)
        XCTAssertEqual(store.bottles.first?.state, .created)
    }
}
