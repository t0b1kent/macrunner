import SwiftUI

@MainActor
final class RunAppViewModel: ObservableObject {
    @Published var settings: AppSettings
    @Published var runner = CommandRunner()
    @Published var lastLauncherResult: LauncherResult?
    @Published var jsonOutputPath: String = ""
    var onComplete: ((LauncherResult?) -> Void)?

    init(settings: AppSettings) {
        self.settings = settings
    }

    func run(app: AppEntry) {
        let root = settings.macRunnerRoot
        let script = "\(root)/scripts/run-windows-app.sh"
        let exe = app.exePath
        var args: [String] = [exe]
        if let to = app.timeout {
            args += ["--timeout", "\(to)"]
        }
        if let wd = app.workdir {
            args += ["--workdir", wd]
        }
        if let appArgs = app.args, !appArgs.isEmpty {
            args += ["--args", appArgs.joined(separator: " ")]
        }
        let backend = app.d3dBackend
        if backend != "none" {
            args += ["--d3d-backend", backend]
        }
        if settings.keepArtifactsDefault {
            args += ["--d3d-keep-artifacts"]
        }
        if settings.enableDebugLogs {
            args += ["--debug"]
        }

        let stamp = ISO8601DateFormatter().string(from: Date()).replacingOccurrences(of: ":", with: "-")
        let artifactsDir = "\(root)/artifacts/control-center/\(stamp)"
        let jsonPath = "\(artifactsDir)/last-run.json"
        args += ["--json", jsonPath]
        jsonOutputPath = jsonPath

        var env = EngineEnv(settings: settings).processEnvironment()
        if let appEnv = app.env {
            for key in appEnv.keys.sorted() {
                guard let value = appEnv[key] else { continue }
                args += ["--env", "\(key)=\(value)"]
                env[key] = value
            }
        }

        let launcherTimeout = app.timeout ?? settings.defaultTimeout
        let runnerTimeout = TimeInterval(launcherTimeout + 90)
        runner.run(command: script, arguments: args, workingDirectory: root, environment: env, timeout: runnerTimeout)

        Task {
            while runner.isRunning {
                try? await Task.sleep(nanoseconds: 200_000_000)
            }
            await MainActor.run {
                self.loadResult()
                if self.lastLauncherResult == nil, let cmdResult = self.runner.lastResult {
                    self.lastLauncherResult = LauncherResult(
                        status: cmdResult.status == .success ? "PASS" : "FAIL",
                        rc: cmdResult.exitCode,
                        durationMs: cmdResult.durationMs,
                        error: cmdResult.stderr.isEmpty ? nil : cmdResult.stderr
                    )
                }
                if let result = self.lastLauncherResult {
                    CompatibilityStore.shared.update(from: app, result: result)
                }
                self.onComplete?(self.lastLauncherResult)
            }
        }
    }

    func cancel() {
        runner.cancel()
    }

    private func loadResult() {
        guard !jsonOutputPath.isEmpty,
              let data = try? Data(contentsOf: URL(fileURLWithPath: jsonOutputPath)) else { return }
        do {
            lastLauncherResult = try JSONDecoder().decode(LauncherResult.self, from: data)
        } catch {
            lastLauncherResult = nil
        }
    }
}
