import Foundation
import Testing
@testable import MacRunnerControlCenter

struct CompatibilityDBTests {
    @Test @MainActor func loadSaveEntries() {
        let store = CompatibilityStore.shared
        let original = store.loadEntries()
        let entry = CompatibilityEntry(
            id: "test-1", name: "Test", exePathHash: "hash", arch: "arm64",
            lastStatus: "PASS", bestD3DBackend: "none", lastSuccessfulVersion: "v0.1",
            failuresCount: 0, notes: nil, tags: nil, lastRunDate: nil, category: nil, artifactPaths: nil
        )
        store.saveEntries(original + [entry])
        let loaded = store.loadEntries()
        #expect(loaded.contains { $0.id == "test-1" })
        store.saveEntries(original)
    }

    @Test @MainActor func historyRoundTrip() {
        let store = CompatibilityStore.shared
        let original = store.loadHistory()
        let h = AppRunHistory(id: UUID(), appId: "a", status: "PASS", durationMs: 100, d3dBackend: "mock", timestamp: Date(), launcherJsonPath: nil)
        store.saveHistory(original + [h])
        let loaded = store.loadHistory()
        #expect(loaded.contains { $0.appId == "a" })
        store.saveHistory(original)
    }

    @Test @MainActor func updateFromApp() {
        let store = CompatibilityStore.shared
        let original = store.loadEntries()
        let app = AppEntry.new(name: "T", exePath: "/tmp/t.exe")
        let result = LauncherResult(schemaVersion: 2, exe: nil, exePath: nil, arch: "arm64", machine: nil, executionLane: nil, status: "PASS", rc: 0, exitCode: nil, stdout: nil, stdoutPath: nil, stderrPath: nil, stderrTail: nil, durationMs: 100, timedOut: false, timeout: false, crashed: false, cleanupOk: true, leftoversCount: 0, d3dEnabled: false, d3dBackend: nil, d3dStatus: nil, d3dTracePath: nil, d3dIrPath: nil, d3dReportPath: nil, d3dPpmPath: nil, d3dOutputChecksum: nil, d3dNonBackgroundPixels: nil, d3dUnsupportedCalls: nil, d3dValidationErrors: nil, metalDeviceDetected: nil, args: nil, envOverrides: nil, workdir: nil, command: nil, error: nil, cleanup: nil)
        store.update(from: app, result: result)
        let loaded = store.loadEntries()
        #expect(loaded.contains { $0.id == app.id.uuidString })
        store.saveEntries(original)
    }

    @Test @MainActor func passRateCalculation() {
        let store = CompatibilityStore.shared
        let original = store.loadHistory()
        defer { store.saveHistory(original) }

        let appId = "pass-rate-test"
        let history = [
            AppRunHistory(id: UUID(), appId: appId, status: "PASS", durationMs: 100, d3dBackend: "mock", timestamp: Date(), launcherJsonPath: nil),
            AppRunHistory(id: UUID(), appId: appId, status: "FAIL", durationMs: 100, d3dBackend: "mock", timestamp: Date().addingTimeInterval(-1), launcherJsonPath: nil),
            AppRunHistory(id: UUID(), appId: appId, status: "PASS", durationMs: 100, d3dBackend: "mock", timestamp: Date().addingTimeInterval(-2), launcherJsonPath: nil),
            AppRunHistory(id: UUID(), appId: appId, status: "PASS", durationMs: 100, d3dBackend: "mock", timestamp: Date().addingTimeInterval(-3), launcherJsonPath: nil)
        ]
        store.saveHistory(original + history)

        let rate = store.passRate(appId: appId)
        #expect(rate == 0.75)
    }

    @Test @MainActor func regressionDetection() {
        let store = CompatibilityStore.shared
        let original = store.loadHistory()
        defer { store.saveHistory(original) }

        let appId = "regression-test"
        let history = [
            AppRunHistory(id: UUID(), appId: appId, status: "FAIL", durationMs: 100, d3dBackend: "mock", timestamp: Date(), launcherJsonPath: nil),
            AppRunHistory(id: UUID(), appId: appId, status: "PASS", durationMs: 100, d3dBackend: "mock", timestamp: Date().addingTimeInterval(-1), launcherJsonPath: nil)
        ]
        store.saveHistory(original + history)

        #expect(store.isRegression(appId: appId) == true)
    }

    @Test @MainActor func noRegressionIfFirstRunFails() {
        let store = CompatibilityStore.shared
        let original = store.loadHistory()
        defer { store.saveHistory(original) }

        let appId = "no-regression-test"
        let history = [
            AppRunHistory(id: UUID(), appId: appId, status: "FAIL", durationMs: 100, d3dBackend: "mock", timestamp: Date(), launcherJsonPath: nil)
        ]
        store.saveHistory(original + history)

        #expect(store.isRegression(appId: appId) == false)
    }

    @Test @MainActor func bestBackendSelection() {
        let store = CompatibilityStore.shared
        let original = store.loadHistory()
        defer { store.saveHistory(original) }

        let appId = "backend-test"
        let history = [
            AppRunHistory(id: UUID(), appId: appId, status: "PASS", durationMs: 100, d3dBackend: "metal", timestamp: Date(), launcherJsonPath: nil),
            AppRunHistory(id: UUID(), appId: appId, status: "PASS", durationMs: 100, d3dBackend: "metal", timestamp: Date().addingTimeInterval(-1), launcherJsonPath: nil),
            AppRunHistory(id: UUID(), appId: appId, status: "FAIL", durationMs: 100, d3dBackend: "mock", timestamp: Date().addingTimeInterval(-2), launcherJsonPath: nil),
            AppRunHistory(id: UUID(), appId: appId, status: "FAIL", durationMs: 100, d3dBackend: "mock", timestamp: Date().addingTimeInterval(-3), launcherJsonPath: nil)
        ]
        store.saveHistory(original + history)

        #expect(store.bestBackend(appId: appId) == "metal")
    }

    @Test @MainActor func regressionManifestGeneration() {
        let store = CompatibilityStore.shared
        let original = store.loadHistory()
        defer { store.saveHistory(original) }

        let appId = "regression-manifest-test"
        let history = [
            AppRunHistory(id: UUID(), appId: appId, status: "FAIL", durationMs: 100, d3dBackend: "mock", timestamp: Date(), launcherJsonPath: nil),
            AppRunHistory(id: UUID(), appId: appId, status: "PASS", durationMs: 100, d3dBackend: "mock", timestamp: Date().addingTimeInterval(-1), launcherJsonPath: nil)
        ]
        store.saveHistory(original + history)

        let entries = [
            CompatibilityEntry(id: appId, name: "TestApp", exePathHash: "/tmp/test.exe", arch: "x64", lastStatus: "FAIL", bestD3DBackend: "mock", lastSuccessfulVersion: nil, failuresCount: 1, notes: nil, tags: nil, lastRunDate: Date(), category: nil, artifactPaths: nil)
        ]
        store.saveEntries(entries)

        let manifest = RegressionManifestService.generateRegressionsManifest(macRunnerRoot: "/tmp")
        #expect(manifest != nil)
        #expect(manifest?.apps?.count == 1)
        #expect(manifest?.apps?.first?.name == "TestApp")
    }

    @Test func statusAllCases() {
        let all = CompatibilityStatus.allCases
        #expect(all.count > 0)
        #expect(all.contains(.runs))
        #expect(all.contains(.crashes))
    }
}
