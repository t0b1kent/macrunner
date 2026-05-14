import SwiftUI

@MainActor
final class DoctorViewModel: ObservableObject {
    @Published var runner = CommandRunner()
    @Published var doctorReport: DoctorReport?
    @Published var verifyReport: PlatformVerifyReport?
    @Published var lastCommand: String = ""
    @Published var history: [String] = []

    var settings: AppSettings

    init(settings: AppSettings) {
        self.settings = settings
    }

    func runDoctor() {
        let script = "\(settings.macRunnerRoot)/scripts/macr-doctor.sh"
        lastCommand = "Doctor"
        doctorReport = nil
        runner.run(command: script, arguments: [], workingDirectory: settings.macRunnerRoot, timeout: TimeInterval(settings.doctorTimeout))
        observeCompletion { [weak self] in
            self?.loadDoctorReport()
        }
    }

    func runQuickVerify() {
        let script = "\(settings.macRunnerRoot)/scripts/verify-native-platform.sh"
        lastCommand = "Quick Verify"
        verifyReport = nil
        runner.run(command: script, arguments: [], workingDirectory: settings.macRunnerRoot, timeout: TimeInterval(settings.integrationTimeout))
        observeCompletion { [weak self] in
            self?.loadVerifyReport()
        }
    }

    func runCheckLeftovers() {
        let script = "\(settings.macRunnerRoot)/scripts/assert-no-wine-leftovers.sh"
        lastCommand = "Check Leftovers"
        runner.run(command: script, arguments: [], workingDirectory: settings.macRunnerRoot, timeout: TimeInterval(settings.doctorTimeout))
    }

    func runCleanup() {
        let script = "\(settings.macRunnerRoot)/scripts/cleanup-wine-runtime.py"
        lastCommand = "Cleanup"
        runner.run(command: "/usr/bin/python3", arguments: [script, "--json", "--sample", "3"], workingDirectory: settings.macRunnerRoot, timeout: TimeInterval(settings.doctorTimeout))
    }

    func enqueueDoctor() {
        TaskQueue.shared.enqueue(
            type: .runDoctor,
            command: "\(settings.macRunnerRoot)/scripts/macr-doctor.sh",
            arguments: [],
            workingDirectory: settings.macRunnerRoot,
            timeout: TimeInterval(settings.doctorTimeout)
        )
    }

    func enqueueVerify() {
        TaskQueue.shared.enqueue(
            type: .runVerify,
            command: "\(settings.macRunnerRoot)/scripts/verify-native-platform.sh",
            arguments: [],
            workingDirectory: settings.macRunnerRoot,
            timeout: TimeInterval(settings.integrationTimeout)
        )
    }

    func exportReport() {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.json]
        panel.nameFieldStringValue = "doctor-export.json"
        if panel.runModal() == .OK, let url = panel.url {
            var export: [String: Any] = [:]
            if let report = doctorReport {
                if let data = try? JSONEncoder().encode(report),
                   let dict = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
                    export["doctor"] = dict
                }
            }
            if let report = verifyReport {
                export["verify"] = ["status": report.status ?? "unknown"]
            }
            if let data = try? JSONSerialization.data(withJSONObject: export, options: .prettyPrinted) {
                try? data.write(to: url)
            }
        }
    }

    var healthScore: Int {
        var score = 0
        let max = 6
        if doctorReport?.lanes?.arm64?.exists == true { score += 1 }
        if doctorReport?.lanes?.x64?.exists == true { score += 1 }
        if doctorReport?.lanes?.x86?.exists == true { score += 1 }
        if doctorReport?.graphics?.metalProbePass == true { score += 1 }
        if doctorReport?.graphics?.renderCore == true { score += 1 }
        if doctorReport?.scripts?.runWindowsApp?.exists == true { score += 1 }
        return Int((Double(score) / Double(max)) * 100)
    }

    var recommendations: [String] {
        var recs: [String] = []
        if doctorReport?.lanes?.arm64?.exists != true {
            recs.append("ARM64 lane is missing. Check that the ARM64 runtime is installed.")
        }
        if doctorReport?.lanes?.x64?.exists != true {
            recs.append("x64 lane is missing. Verify x64 cross-compilation tools.")
        }
        if doctorReport?.lanes?.x86?.exists != true {
            recs.append("x86 lane is missing. x86 compatibility layer may need installation.")
        }
        if doctorReport?.graphics?.metalProbePass != true {
            recs.append("Metal probe failed. Ensure macOS Metal drivers are up to date.")
        }
        if doctorReport?.graphics?.renderCore != true {
            recs.append("D3D render core is not available. Re-run setup to build graphics components.")
        }
        if doctorReport?.scripts?.runWindowsApp?.exists != true {
            recs.append("run-windows-app.sh script is missing. Check MacRunner installation.")
        }
        if doctorReport?.tools?.clang != true {
            recs.append("Clang is not available. Install Xcode Command Line Tools.")
        }
        if doctorReport?.tools?.python == nil {
            recs.append("Python is not available. Some scripts require Python 3.")
        }
        return recs
    }

    private func observeCompletion(completion: @escaping @MainActor () -> Void) {
        Task { @MainActor in
            while runner.isRunning {
                try? await Task.sleep(nanoseconds: 200_000_000)
            }
            completion()
        }
    }

    private func loadDoctorReport() {
        let path = "\(settings.macRunnerRoot)/reports/macr-doctor.json"
        guard let data = try? Data(contentsOf: URL(fileURLWithPath: path)) else { return }
        do {
            doctorReport = try JSONDecoder().decode(DoctorReport.self, from: data)
            history.insert("Doctor: \(doctorReport?.host?.macos ?? "?")", at: 0)
            if history.count > 10 { history = Array(history.prefix(10)) }
        } catch {
            doctorReport = nil
        }
    }

    private func loadVerifyReport() {
        let path = "\(settings.macRunnerRoot)/reports/native-platform-verify.json"
        guard let data = try? Data(contentsOf: URL(fileURLWithPath: path)) else { return }
        do {
            verifyReport = try JSONDecoder().decode(PlatformVerifyReport.self, from: data)
        } catch {
            verifyReport = nil
        }
    }
}
