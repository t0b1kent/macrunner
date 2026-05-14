import SwiftUI

@MainActor
final class CorpusViewModel: ObservableObject {
    @Published var manifests: [RealAppManifest] = []
    @Published var runner = CommandRunner()
    @Published var resultsPath: String?
    @Published var summaryPath: String?
    @Published var search = ""
    @Published var batchProgress: (completed: Int, total: Int)?
    @Published var lastBatchResults: [String: String] = [:]
    @Published var selectedManifest: RealAppManifest?

    var settings: AppSettings

    var filteredApps: [(manifest: RealAppManifest, app: ManifestApp)] {
        var all: [(RealAppManifest, ManifestApp)] = []
        for m in manifests {
            for app in m.apps ?? [] {
                all.append((m, app))
            }
        }
        if search.isEmpty { return all }
        return all.filter {
            ($0.1.name ?? "").localizedCaseInsensitiveContains(search)
                || ($0.1.path ?? "").localizedCaseInsensitiveContains(search)
                || ($0.1.arch ?? "").localizedCaseInsensitiveContains(search)
        }
    }

    var stats: (total: Int, pass: Int, fail: Int) {
        let total = filteredApps.count
        let pass = lastBatchResults.values.filter { $0 == "PASS" }.count
        let fail = lastBatchResults.values.filter { $0 != "PASS" }.count
        return (total, pass, fail)
    }

    init(settings: AppSettings) {
        self.settings = settings
    }

    func refresh() {
        let dir = "\(settings.macRunnerRoot)/tests/real-app-manifests"
        var list: [RealAppManifest] = []
        let fm = FileManager.default
        guard let files = try? fm.contentsOfDirectory(atPath: dir) else {
            manifests = []
            return
        }
        for file in files where file.hasSuffix(".json") {
            let path = "\(dir)/\(file)"
            guard let data = try? Data(contentsOf: URL(fileURLWithPath: path)) else { continue }
            if let manifest = try? JSONDecoder().decode(RealAppManifest.self, from: data) {
                list.append(manifest)
            }
        }
        manifests = list
    }

    func runAll() {
        let script = "\(settings.macRunnerRoot)/scripts/run-real-app-corpus.sh"
        let dir = "\(settings.macRunnerRoot)/tests/real-app-manifests"
        runner.run(command: script, arguments: [dir], workingDirectory: settings.macRunnerRoot, timeout: TimeInterval(settings.integrationTimeout))
        observeResults()
    }

    func runManifest(_ manifest: RealAppManifest) {
        guard let app = manifest.apps?.first, let path = app.path else { return }
        let script = "\(settings.macRunnerRoot)/scripts/run-windows-app.sh"
        var args = [path]
        if let to = app.timeout {
            args += ["--timeout", "\(to)"]
        }
        if let backend = app.d3dBackend, backend != "none" {
            args += ["--d3d-backend", backend]
        }
        runner.run(command: script, arguments: args, workingDirectory: settings.macRunnerRoot, timeout: TimeInterval(app.timeout ?? settings.defaultTimeout))
    }

    func runApp(_ app: ManifestApp) {
        guard let path = app.path else { return }
        let script = "\(settings.macRunnerRoot)/scripts/run-windows-app.sh"
        var args = [path]
        if let to = app.timeout {
            args += ["--timeout", "\(to)"]
        }
        if let backend = app.d3dBackend, backend != "none" {
            args += ["--d3d-backend", backend]
        }
        runner.run(command: script, arguments: args, workingDirectory: settings.macRunnerRoot, timeout: TimeInterval(app.timeout ?? settings.defaultTimeout))
        observeSingleResult(for: app)
    }

    func runBatch() {
        let apps = filteredApps.map { $0.app }
        guard !apps.isEmpty else { return }
        batchProgress = (0, apps.count)
        lastBatchResults = [:]

        Task {
            for (idx, app) in apps.enumerated() {
                guard let path = app.path else { continue }
                let script = "\(settings.macRunnerRoot)/scripts/run-windows-app.sh"
                var args = [path]
                if let to = app.timeout {
                    args += ["--timeout", "\(to)"]
                }
                if let backend = app.d3dBackend, backend != "none" {
                    args += ["--d3d-backend", backend]
                }

                let localRunner = CommandRunner()
                await MainActor.run {
                    localRunner.run(command: script, arguments: args, workingDirectory: self.settings.macRunnerRoot, timeout: TimeInterval(app.timeout ?? self.settings.defaultTimeout))
                }

                while localRunner.isRunning {
                    try? await Task.sleep(nanoseconds: 200_000_000)
                }

                let status = localRunner.lastResult?.status == .success ? "PASS" : "FAIL"
                await MainActor.run {
                    self.lastBatchResults[app.id.uuidString] = status
                    self.batchProgress = (idx + 1, apps.count)
                    self.saveBatchResultToCompatibility(app: app, status: status)
                }
            }
            await MainActor.run {
                self.batchProgress = nil
            }
        }
    }

    private func saveBatchResultToCompatibility(app: ManifestApp, status: String) {
        let apps = ConfigStore.shared.loadApps()
        if let entry = apps.first(where: { $0.exePath == app.path }) {
            var updated = entry
            updated.lastRunStatus = status
            updated.updatedAt = Date()
            ConfigStore.shared.saveApps(apps.map { $0.id == updated.id ? updated : $0 })
            let result = LauncherResult(status: status)
            CompatibilityStore.shared.update(from: updated, result: result)
        }
    }

    func exportManifest(_ manifest: RealAppManifest) {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.json]
        panel.nameFieldStringValue = "manifest.json"
        if panel.runModal() == .OK, let url = panel.url,
           let data = try? JSONEncoder().encode(manifest) {
            try? data.write(to: url)
        }
    }

    func deleteManifest(_ manifest: RealAppManifest) {
        let dir = "\(settings.macRunnerRoot)/tests/real-app-manifests"
        let fm = FileManager.default
        guard let files = try? fm.contentsOfDirectory(atPath: dir) else { return }
        for file in files where file.hasSuffix(".json") {
            let path = "\(dir)/\(file)"
            if let data = try? Data(contentsOf: URL(fileURLWithPath: path)),
               let m = try? JSONDecoder().decode(RealAppManifest.self, from: data),
               m.apps?.first?.name == manifest.apps?.first?.name {
                try? fm.removeItem(atPath: path)
            }
        }
        refresh()
    }

    private func observeResults() {
        Task {
            while runner.isRunning { try? await Task.sleep(nanoseconds: 200_000_000) }
            await MainActor.run {
                let reportsDir = "\(self.settings.macRunnerRoot)/reports"
                self.resultsPath = "\(reportsDir)/real-app-corpus.jsonl"
                self.summaryPath = "\(reportsDir)/REAL-RUNTIME-VERIFICATION.md"
            }
        }
    }

    private func observeSingleResult(for app: ManifestApp) {
        Task {
            while runner.isRunning { try? await Task.sleep(nanoseconds: 200_000_000) }
            await MainActor.run {
                let status = self.runner.lastResult?.status == .success ? "PASS" : "FAIL"
                self.lastBatchResults[app.id.uuidString] = status
            }
        }
    }
}
