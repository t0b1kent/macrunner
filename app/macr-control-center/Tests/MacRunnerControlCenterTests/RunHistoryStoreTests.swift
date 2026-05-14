import Foundation
import Testing
@testable import MacRunnerControlCenter

struct RunHistoryStoreTests {
    @Test @MainActor func appendAndLoad() {
        let store = RunHistoryStore()
        store.clear()

        let task = MacRunnerTask(type: .runDoctor, command: "/bin/echo", arguments: ["test"])
        store.append(task)

        let loaded = store.load()
        #expect(loaded.count == 1)
        #expect(loaded[0].command == "/bin/echo")
    }

    @Test @MainActor func removeTask() {
        let store = RunHistoryStore()
        store.clear()

        let task = MacRunnerTask(type: .runVerify, command: "/bin/echo")
        store.append(task)
        store.remove(taskId: task.id)

        #expect(store.load().isEmpty)
    }

    @Test @MainActor func maxEntries() {
        let store = RunHistoryStore()
        store.clear()

        for i in 0..<250 {
            let task = MacRunnerTask(type: .customCommand, command: "/bin/echo", arguments: ["\(i)"])
            store.append(task)
        }

        let loaded = store.load()
        #expect(loaded.count <= 200)
    }
}
