import Foundation
import Testing
@testable import MacRunnerControlCenter

struct SettingsTests {
    @Test @MainActor func defaultSettings() {
        let s = AppSettings.default
        #expect(s.macRunnerRoot == "/Users/timurtoby/Documents/MacRunner/Main/MacRunner")
        #expect(s.defaultTimeout == 45)
        #expect(s.defaultD3DBackend == "none")
        #expect(s.bottlesDirectory == "/Users/timurtoby/Documents/MacRunner/Main/MacRunner/bottles")
    }

    @Test @MainActor func persistAndLoad() throws {
        let store = ConfigStore.shared
        let original = store.loadSettings()
        let test = AppSettings(
            macRunnerRoot: "/tmp/test-macrunner",
            defaultTimeout: 10,
            defaultD3DBackend: "mock",
            keepArtifactsDefault: true,
            doctorTimeout: 30,
            integrationTimeout: 60,
            artifactsDirectory: "/tmp/artifacts",
            bottlesDirectory: "~/tmp/bottles",
            enableDebugLogs: true,
            enableMockMode: true
        )
        store.saveSettings(test)
        let loaded = store.loadSettings()
        #expect(loaded.macRunnerRoot == test.macRunnerRoot)
        #expect(loaded.defaultTimeout == test.defaultTimeout)
        store.saveSettings(original)
    }

    @Test func settingsEncodeDecode() throws {
        let original = AppSettings.default
        let data = try JSONEncoder().encode(original)
        let decoded = try JSONDecoder().decode(AppSettings.self, from: data)
        #expect(decoded.macRunnerRoot == original.macRunnerRoot)
        #expect(decoded.defaultTimeout == original.defaultTimeout)
        #expect(decoded.enableMockMode == original.enableMockMode)
    }
}
