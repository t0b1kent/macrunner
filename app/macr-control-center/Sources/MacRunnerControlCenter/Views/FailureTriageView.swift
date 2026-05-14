import SwiftUI

struct FailureTriageView: View {
    let result: LauncherResult?
    let exePath: String?
    @EnvironmentObject var settingsVM: SettingsViewModel
    @State private var diagnosis: FailureClassifier.Diagnosis?
    @State private var showDetail = false

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Image(systemName: "stethoscope")
                    .foregroundStyle(Color.accentColor)
                Text("Failure Triage")
                    .font(.headline)
                Spacer()
                if let d = diagnosis {
                    Text("\(Int(d.confidence * 100))% confidence")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }

            if let d = diagnosis {
                HStack {
                    Text(d.classification.rawValue)
                        .font(.callout.bold())
                        .foregroundStyle(color(for: d.classification))
                    Spacer()
                }

                Text(d.cause)
                    .font(.callout)

                if !d.evidence.isEmpty {
                    Text(d.evidence)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .textSelection(.enabled)
                }

                HStack {
                    Button("View Detail") { showDetail = true }
                    Button("Copy Codex Prompt") {
                        let prompt = CodexPromptService.generateFixPrompt(result: result, diagnosis: diagnosis)
                        NSPasteboard.general.clearContents()
                        NSPasteboard.general.setString(prompt, forType: .string)
                    }
                    Spacer()
                    if d.actions.contains("Export Debug Bundle") {
                        Button("Export Bundle") {
                            if let url = DebugBundleExporter.export(
                                settings: settingsVM.settings,
                                lastLauncherPath: result?.stdoutPath
                            ) {
                                NSWorkspace.shared.open(url)
                            }
                        }
                    }
                    if d.actions.contains("Run Doctor") {
                        Button("Run Doctor") {
                            let script = "\(settingsVM.settings.macRunnerRoot)/scripts/macr-doctor.sh"
                            TaskQueue.shared.enqueue(
                                type: .runDoctor,
                                command: script,
                                arguments: ["--json"],
                                workingDirectory: settingsVM.settings.macRunnerRoot,
                                timeout: TimeInterval(settingsVM.settings.doctorTimeout)
                            )
                        }
                    }
                    if d.actions.contains("Run D3D Smoke Test") {
                        Button("D3D Smoke") {
                            let script = "\(settingsVM.settings.macRunnerRoot)/scripts/run-windows-app.sh"
                            TaskQueue.shared.enqueue(
                                type: .runD3DSmoke,
                                command: script,
                                arguments: ["--d3d-smoke", "--json", "--timeout", "120"],
                                workingDirectory: settingsVM.settings.macRunnerRoot,
                                timeout: 150
                            )
                        }
                    }
                }
                .controlSize(.small)
            } else {
                ContentUnavailableView("No result to triage", systemImage: "stethoscope")
            }
        }
        .padding()
        .background(Color(.controlBackgroundColor))
        .cornerRadius(8)
        .sheet(isPresented: $showDetail) {
            if let d = diagnosis {
                TriageDetailView(diagnosis: d)
            }
        }
        .onAppear { refresh() }
        .onChange(of: result) { _, _ in refresh() }
    }

    private func refresh() {
        diagnosis = FailureClassifier.classifyWithHistory(result: result, exePath: exePath)
    }

    func color(for cls: FailureClassifier.FailureClass) -> Color {
        switch cls {
        case .invalidExe, .missingDll, .prefixBroken: return .orange
        case .crash, .d3dValidationError, .cleanupFailed: return .red
        case .timeout: return .yellow
        case .d3dShimNotLoaded, .metalUnavailable, .archRoutingError: return .purple
        case .unknown: return .gray
        }
    }
}
