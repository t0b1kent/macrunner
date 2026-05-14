import Foundation
import Testing
@testable import MacRunnerControlCenter

struct CoreStatusTests {
    @Test func unknownDefaults() {
        let s = CoreStatus.unknown(root: "/tmp/test")
        #expect(!s.isRootValid)
        #expect(s.isConnected == false)
        #expect(s.missingScripts.isEmpty)
        #expect(s.missingReportsDir == true)
        #expect(s.missingArtifactsDir == true)
    }

    @Test func checkRealRoot() {
        let status = CoreStatusService.check(root: AppSettings.defaultRoot)
        #expect(status.isRootValid == true)
        #expect(status.mode == .localCore)
        #expect(status.healthScore > 0)
    }

    @Test func checkInvalidRoot() {
        let status = CoreStatusService.check(root: "/nonexistent/path")
        #expect(status.isRootValid == false)
        #expect(status.mode == .mock)
        #expect(!status.isConnected)
    }

    @Test func requiredScriptsList() {
        #expect(CoreStatusService.requiredScripts.contains("scripts/run-windows-app.sh"))
        #expect(CoreStatusService.requiredScripts.contains("scripts/macr-doctor.sh"))
    }
}
