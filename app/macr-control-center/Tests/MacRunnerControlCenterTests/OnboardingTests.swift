import Foundation
import Testing
@testable import MacRunnerControlCenter

struct OnboardingTests {
    @Test @MainActor func defaultSettingsValid() {
        let s = AppSettings.default
        #expect(!s.macRunnerRoot.isEmpty)
    }

    @Test @MainActor func settingsPersistAfterOnboardingSimulation() {
        let store = ConfigStore.shared
        let original = store.loadSettings()
        let test = AppSettings(
            macRunnerRoot: "/tmp/onboarding-test",
            defaultTimeout: 60,
            defaultD3DBackend: "metal",
            keepArtifactsDefault: true,
            doctorTimeout: 90,
            integrationTimeout: 120,
            artifactsDirectory: "/tmp/artifacts",
            bottlesDirectory: "~/tmp/bottles",
            enableDebugLogs: false,
            enableMockMode: true
        )
        store.saveSettings(test)
        let loaded = store.loadSettings()
        #expect(loaded.defaultD3DBackend == "metal")
        store.saveSettings(original)
    }
}
