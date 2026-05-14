import SwiftUI

@MainActor
final class WineProcessViewModel: ObservableObject {
    @Published var runner = CommandRunner()
    @Published var processes: [WineProcess] = []
    @Published var error: String?
    @Published var search = ""
    @Published var autoRefresh = false
    @Published var selectedProcess: WineProcess?

    var settings: AppSettings
    private var timer: Timer?

    var filtered: [WineProcess] {
        if search.isEmpty { return processes }
        return processes.filter {
            ($0.command ?? "").localizedCaseInsensitiveContains(search)
                || ($0.matchedReason ?? "").localizedCaseInsensitiveContains(search)
                || String($0.pid ?? 0).contains(search)
        }
    }

    init(settings: AppSettings) {
        self.settings = settings
    }

    func refresh() {
        let script = "\(settings.macRunnerRoot)/scripts/list-wine-processes.sh"
        if FileManager.default.fileExists(atPath: script) {
            runner.run(command: script, arguments: [], workingDirectory: settings.macRunnerRoot, timeout: 30)
        } else {
            fallbackList()
        }
        observeStdout()
    }

    func cleanupRuntime() {
        let script = "\(settings.macRunnerRoot)/scripts/cleanup-wine-runtime.py"
        runner.run(command: "/usr/bin/python3", arguments: [script, "--json", "--sample", "3"], workingDirectory: settings.macRunnerRoot, timeout: 60)
        observeStdout()
    }

    func assertNoLeftovers() {
        let script = "\(settings.macRunnerRoot)/scripts/assert-no-wine-leftovers.sh"
        runner.run(command: script, arguments: [], workingDirectory: settings.macRunnerRoot, timeout: 60)
    }

    func killProcess(_ proc: WineProcess) {
        guard let pid = proc.pid else { return }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/kill")
        p.arguments = ["-9", "\(pid)"]
        try? p.run()
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
            self.refresh()
        }
    }

    func setAutoRefresh(_ enabled: Bool) {
        autoRefresh = enabled
        timer?.invalidate()
        if enabled {
            timer = Timer.scheduledTimer(withTimeInterval: 3.0, repeats: true) { _ in
                Task { @MainActor in
                    self.refresh()
                }
            }
        }
    }

    func stopAutoRefresh() {
        timer?.invalidate()
        timer = nil
        autoRefresh = false
    }

    deinit {
        timer?.invalidate()
    }

    private func fallbackList() {
        runner.run(command: "/bin/ps", arguments: ["aux"], timeout: 10)
        Task {
            while runner.isRunning { try? await Task.sleep(nanoseconds: 200_000_000) }
            await MainActor.run {
                let lines = self.runner.lastResult?.stdout.split(separator: "\n") ?? []
                var procs: [WineProcess] = []
                for line in lines {
                    let parts = line.split(separator: " ", omittingEmptySubsequences: true)
                    if parts.count > 10, line.lowercased().contains("wine") {
                        procs.append(WineProcess(
                            pid: Int(parts[1]),
                            command: String(parts[10...].joined(separator: " ")),
                            age: String(parts[9]),
                            ppid: nil,
                            matchedReason: "wine keyword",
                            status: "unknown"
                        ))
                    }
                }
                self.processes = procs
            }
        }
    }

    private func observeStdout() {
        Task {
            while runner.isRunning { try? await Task.sleep(nanoseconds: 200_000_000) }
            await MainActor.run {
                if let data = self.runner.lastResult?.stdout.data(using: .utf8) {
                    do {
                        let report = try JSONDecoder().decode(ProcessReport.self, from: data)
                        self.processes = report.processes ?? []
                    } catch {
                        self.error = "Failed to parse process list"
                    }
                }
            }
        }
    }
}
