import Foundation
import Testing
@testable import MacRunnerControlCenter

struct ManifestTests {
    @Test func encodeDecodeManifest() throws {
        let app = ManifestApp(
            name: "Test",
            path: "/tmp/test.exe",
            arch: "arm64",
            args: ["a", "b"],
            env: nil,
            workdir: "/tmp",
            expectedRc: 0,
            expectedStdoutContains: "ok",
            expectedFiles: nil,
            d3dBackend: "mock",
            allowGui: false,
            timeout: 20
        )
        let manifest = RealAppManifest(schemaVersion: 1, apps: [app])
        let data = try JSONEncoder().encode(manifest)
        let decoded = try JSONDecoder().decode(RealAppManifest.self, from: data)
        #expect(decoded.apps?.first?.name == "Test")
        #expect(decoded.apps?.first?.d3dBackend == "mock")
    }
}
