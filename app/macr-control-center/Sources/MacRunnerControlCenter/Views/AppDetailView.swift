import SwiftUI

struct AppDetailView: View {
    let app: AppEntry
    let onUpdate: (AppEntry) -> Void
    @EnvironmentObject var settingsVM: SettingsViewModel
    @StateObject private var runVM = RunAppViewModel(settings: .default)
    @State private var showEdit = false
    @State private var runHistory: [AppRunHistory] = []
    @State private var showConfirmDelete = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                HStack {
                    Text(app.name).font(.title)
                    Spacer()
                    StatusBadge(status: app.lastRunStatus)
                }

                InfoRow(label: "Path", value: app.exePath)
                InfoRow(label: "Architecture", value: app.arch ?? "Unknown")
                InfoRow(label: "Args", value: app.args?.joined(separator: " ") ?? "")
                InfoRow(label: "Workdir", value: app.workdir ?? "")
                InfoRow(label: "D3D Backend", value: app.d3dBackend)
                InfoRow(label: "Timeout", value: "\(app.timeout ?? 0)")
                InfoRow(label: "Tags", value: app.tags?.joined(separator: ", ") ?? "")
                InfoRow(label: "Notes", value: app.notes ?? "")

                if let ms = app.lastDurationMs {
                    InfoRow(label: "Last Duration", value: "\(ms) ms")
                }

                HStack {
                    Button("Run") {
                        runVM.run(app: app)
                    }
                    .disabled(runVM.runner.isRunning)

                    Button("Cancel") {
                        runVM.cancel()
                    }
                    .disabled(!runVM.runner.isRunning)

                    Button("Edit") { showEdit = true }

                    Button("Export Debug Bundle") {
                        if let url = DebugBundleExporter.export(
                            settings: settingsVM.settings,
                            lastLauncherPath: runVM.jsonOutputPath
                        ) {
                            NSWorkspace.shared.open(url)
                        }
                    }
                }

                if runVM.runner.isRunning {
                    ProgressView().padding(.vertical, 4)
                }

                if !runVM.runner.stdoutBuffer.isEmpty {
                    Text("Stdout").font(.headline)
                    LogTextView(text: runVM.runner.stdoutBuffer)
                }
                if !runVM.runner.stderrBuffer.isEmpty {
                    Text("Stderr").font(.headline)
                    LogTextView(text: runVM.runner.stderrBuffer)
                        .foregroundStyle(.red)
                }

                if let result = runVM.lastLauncherResult {
                    LauncherResultView(result: result)
                    if result.status != "PASS" {
                        FailureTriageView(result: result, exePath: app.exePath)
                    }
                    ArtifactStatusSection(result: result)
                    if result.d3dEnabled == true || result.d3dStatus != nil {
                        InlineD3DArtifactsView(result: result)
                    }
                }

                RunHistorySection(appId: app.id.uuidString, history: runHistory)
            }
            .padding()
        }
        .sheet(isPresented: $showEdit) {
            AddAppView(app: app, onSave: { onUpdate($0); showEdit = false })
        }
        .onAppear {
            runVM.settings = settingsVM.settings
            refreshHistory()
        }
        .onChange(of: runVM.lastLauncherResult) { _, newValue in
            if let result = newValue {
                var updated = app
                updated.lastRunStatus = result.status
                updated.lastDurationMs = result.durationMs
                updated.updatedAt = Date()
                onUpdate(updated)
                refreshHistory()
            }
        }
    }

    private func refreshHistory() {
        runHistory = CompatibilityStore.shared.loadHistory()
            .filter { $0.appId == app.id.uuidString }
            .sorted { $0.timestamp > $1.timestamp }
    }
}

struct InfoRow: View {
    let label: String
    let value: String
    var body: some View {
        HStack(alignment: .top) {
            Text(label).fontWeight(.semibold).frame(width: 120, alignment: .leading)
            Text(value).textSelection(.enabled)
            Spacer()
        }
    }
}

struct StatusBadge: View {
    let status: String?
    var body: some View {
        Text(status ?? "—")
            .font(.caption.bold())
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
            .background(color)
            .foregroundStyle(.white)
            .clipShape(Capsule())
    }

    var color: Color {
        switch status {
        case "PASS": return .green
        case "FAIL": return .red
        case "CRASH": return .orange
        case "TIMEOUT": return .yellow
        default: return .gray
        }
    }
}

struct LogTextView: View {
    let text: String
    var body: some View {
        Text(text)
            .font(.system(.caption, design: .monospaced))
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(8)
            .background(Color(.textBackgroundColor))
            .cornerRadius(6)
    }
}

struct LauncherResultView: View {
    let result: LauncherResult
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Last Run Result").font(.headline)
            InfoRow(label: "Status", value: result.status ?? "?")
            InfoRow(label: "Exit Code", value: "\(result.rc ?? result.exitCode ?? -1)")
            InfoRow(label: "Duration", value: "\(result.durationMs ?? 0) ms")
            InfoRow(label: "Arch", value: result.arch ?? result.machine ?? "?")
            InfoRow(label: "Cleanup OK", value: (result.cleanupOk ?? false) ? "Yes" : "No")
            if let lc = result.leftoversCount, lc > 0 {
                InfoRow(label: "Leftovers", value: "\(lc)").foregroundStyle(.red)
            }
            if let err = result.error, !err.isEmpty {
                InfoRow(label: "Error", value: err).foregroundStyle(.red)
            }
        }
    }
}

private struct ArtifactStatusSection: View {
    let result: LauncherResult

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Artifacts").font(.headline)

            if let ppm = result.d3dPpmPath {
                HStack {
                    Text("PPM Image")
                    Spacer()
                    if FileManager.default.fileExists(atPath: ppm) {
                        PPMPreview(path: ppm)
                            .frame(width: 120, height: 120)
                    } else {
                        Label("Missing", systemImage: "xmark.circle.fill")
                            .foregroundStyle(.secondary)
                            .font(.caption)
                    }
                }
            } else {
                ArtifactStatusRow(label: "PPM Image", path: nil)
            }

            ArtifactStatusRow(label: "D3D Trace", path: result.d3dTracePath)
            ArtifactStatusRow(label: "D3D Report", path: result.d3dReportPath)
            ArtifactStatusRow(label: "D3D IR", path: result.d3dIrPath)
            ArtifactStatusRow(label: "Stdout File", path: result.stdoutPath)
            ArtifactStatusRow(label: "Stderr File", path: result.stderrPath)
        }
    }
}

private struct ArtifactStatusRow: View {
    let label: String
    let path: String?

    var body: some View {
        HStack {
            Text(label)
            Spacer()
            if let path = path, FileManager.default.fileExists(atPath: path) {
                Label("Present", systemImage: "checkmark.circle.fill")
                    .foregroundStyle(.green)
                    .font(.caption)
            } else {
                Label("Missing", systemImage: "xmark.circle.fill")
                    .foregroundStyle(.secondary)
                    .font(.caption)
            }
        }
    }
}

private struct RunHistorySection: View {
    let appId: String
    let history: [AppRunHistory]

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Run History").font(.headline)
            if history.isEmpty {
                Text("No runs recorded")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } else {
                ForEach(Array(history.prefix(5))) { entry in
                    HStack {
                        Text(entry.timestamp, style: .date)
                            .font(.caption)
                        Spacer()
                        StatusBadge(status: entry.status)
                        if entry.durationMs > 0 {
                            Text("\(entry.durationMs)ms")
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                        }
                    }
                }
                if history.count > 5 {
                    Text("… and \(history.count - 5) more")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
        }
    }
}
