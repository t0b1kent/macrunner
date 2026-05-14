import Foundation

@MainActor
struct RegressionManifestService {
    static func generateRegressionsManifest(macRunnerRoot: String) -> RealAppManifest? {
        let store = CompatibilityStore.shared
        let entries = store.loadEntries()
        let regressed = entries.filter { store.isRegression(appId: $0.id) }
        guard !regressed.isEmpty else { return nil }

        var apps: [ManifestApp] = []
        for entry in regressed {
            let app = ManifestApp(
                name: entry.name,
                path: entry.exePathHash,
                arch: entry.arch,
                args: nil,
                env: nil,
                workdir: nil,
                expectedRc: nil,
                expectedStdoutContains: nil,
                expectedFiles: nil,
                d3dBackend: entry.bestD3DBackend,
                allowGui: nil,
                timeout: 120
            )
            apps.append(app)
        }

        return RealAppManifest(schemaVersion: 1, apps: apps)
    }

    static func saveRegressionsManifest(macRunnerRoot: String) -> String? {
        guard let manifest = generateRegressionsManifest(macRunnerRoot: macRunnerRoot) else { return nil }
        let dir = "\(macRunnerRoot)/tests/real-app-manifests"
        let path = "\(dir)/regressions-\(ISO8601DateFormatter().string(from: Date()).replacingOccurrences(of: ":", with: "-")).json"
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        guard let data = try? JSONEncoder().encode(manifest) else { return nil }
        try? data.write(to: URL(fileURLWithPath: path))
        return path
    }
}
