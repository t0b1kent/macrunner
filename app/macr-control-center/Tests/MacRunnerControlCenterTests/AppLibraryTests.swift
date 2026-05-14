import Foundation
import Testing
@testable import MacRunnerControlCenter

@Suite(.serialized)
struct AppLibraryTests {
    @Test @MainActor func addUpdateDelete() {
        let store = ConfigStore.shared
        let original = store.loadApps()

        let app = AppEntry.new(name: "Test", exePath: "/tmp/test.exe")
        store.saveApps(original + [app])

        var loaded = store.loadApps()
        #expect(loaded.contains(where: { $0.exePath == "/tmp/test.exe" }))

        loaded.removeAll { $0.exePath == "/tmp/test.exe" }
        store.saveApps(loaded)
        loaded = store.loadApps()
        #expect(!loaded.contains(where: { $0.exePath == "/tmp/test.exe" }))
        store.saveApps(original)
    }

    @Test @MainActor func quickRunUpdatesAppStatus() async throws {
        let vm = AppLibraryViewModel()
        let app = AppEntry.new(name: "QuickRunTest", exePath: "/tmp/quickrun.exe")
        vm.add(app)

        var settings = AppSettings.default
        settings.macRunnerRoot = "/tmp/nonexistent-macrunner"

        vm.quickRun(app, settings: settings)

        // Allow time for the failed run to complete
        try? await Task.sleep(nanoseconds: 300_000_000)

        let updated = vm.apps.first { $0.id == app.id }
        #expect(updated?.lastRunStatus == "FAIL")
        #expect(updated?.updatedAt != app.updatedAt)

        // Clean up
        vm.delete(app)
    }

    @Test @MainActor func runAppViewModelOnCompleteCalled() async throws {
        var settings = AppSettings.default
        settings.macRunnerRoot = "/tmp/nonexistent-macrunner"
        let vm = RunAppViewModel(settings: settings)

        let app = AppEntry.new(name: "OnCompleteTest", exePath: "/tmp/oncomplete.exe")
        var receivedResult: LauncherResult?
        vm.onComplete = { result in
            receivedResult = result
        }

        vm.run(app: app)

        try? await Task.sleep(nanoseconds: 300_000_000)

        #expect(receivedResult != nil)
        #expect(receivedResult?.status == "FAIL")
    }
}
