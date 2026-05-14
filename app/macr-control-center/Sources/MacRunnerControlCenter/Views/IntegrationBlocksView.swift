import SwiftUI

struct IntegrationBlocksView: View {
    @EnvironmentObject var settingsVM: SettingsViewModel
    @StateObject private var runner = CommandRunner()
    @State private var results: [IntegrationBlockResult] = []

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Text("Integration Blocks")
                    .font(.headline)
                Spacer()
                Button("Run Blocks") { runBlocks() }
                    .disabled(runner.isRunning)
                Button("Cancel") { runner.cancel() }
                    .disabled(!runner.isRunning)
                if runner.isRunning {
                    ProgressView().padding(.leading, 8)
                }
            }
            .padding()

            if results.isEmpty {
                ContentUnavailableView("No results", systemImage: "building.blocks")
                    .frame(maxHeight: .infinity)
            } else {
                List(results) { result in
                    HStack {
                        Image(systemName: result.passed ? "checkmark.circle.fill" : "xmark.circle.fill")
                            .foregroundStyle(result.passed ? .green : .red)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(result.name)
                                .font(.headline)
                            if let detail = result.detail {
                                Text(detail)
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                                    .lineLimit(2)
                            }
                        }
                        Spacer()
                        Text(result.durationMs > 0 ? "\(result.durationMs)ms" : "—")
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                    }
                }
            }
        }
        .onAppear {
            parseExistingResults()
        }
    }

    private func runBlocks() {
        let script = "\(settingsVM.settings.macRunnerRoot)/scripts/run-integration-blocks.sh"
        guard FileManager.default.fileExists(atPath: script) else {
            results = [IntegrationBlockResult(name: "Script Missing", passed: false, detail: "\(script) not found", durationMs: 0)]
            return
        }
        runner.run(command: script, arguments: ["--json"], workingDirectory: settingsVM.settings.macRunnerRoot, timeout: TimeInterval(settingsVM.settings.integrationTimeout))
        observeResults()
    }

    private func observeResults() {
        Task {
            while runner.isRunning {
                try? await Task.sleep(nanoseconds: 200_000_000)
            }
            await MainActor.run {
                self.parseExistingResults()
            }
        }
    }

    private func parseExistingResults() {
        let path = "\(settingsVM.settings.macRunnerRoot)/reports/integration-blocks.json"
        guard let data = try? Data(contentsOf: URL(fileURLWithPath: path)),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]] else {
            return
        }
        results = obj.compactMap { dict in
            guard let name = dict["name"] as? String else { return nil }
            return IntegrationBlockResult(
                name: name,
                passed: dict["passed"] as? Bool ?? false,
                detail: dict["detail"] as? String ?? dict["error"] as? String,
                durationMs: dict["duration_ms"] as? Int ?? 0
            )
        }
    }
}

struct IntegrationBlockResult: Identifiable, Sendable {
    let id = UUID()
    var name: String
    var passed: Bool
    var detail: String?
    var durationMs: Int
}
