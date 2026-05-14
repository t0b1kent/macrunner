import SwiftUI

struct CorpusView: View {
    @EnvironmentObject var settingsVM: SettingsViewModel
    @StateObject private var vm: CorpusViewModel
    @State private var showEditor = false
    @State private var editingManifest: RealAppManifest?

    init() {
        self._vm = StateObject(wrappedValue: CorpusViewModel(settings: SettingsViewModel().settings))
    }

    var body: some View {
        VStack {
            HStack {
                Button("Refresh") { vm.refresh() }
                Button("Run All") { vm.runAll() }
                Button("Run Batch") { vm.runBatch() }
                    .disabled(vm.filteredApps.isEmpty || vm.batchProgress != nil)
                Button("New Manifest") {
                    editingManifest = RealAppManifest(schemaVersion: 1, apps: [ManifestApp(name: "", path: "", timeout: 45)])
                    showEditor = true
                }
                if let progress = vm.batchProgress {
                    ProgressView(value: Double(progress.completed), total: Double(progress.total))
                        .frame(width: 100)
                    Text("\(progress.completed)/\(progress.total)")
                        .font(.caption)
                        .monospacedDigit()
                }
                if vm.runner.isRunning {
                    ProgressView().padding(.leading, 8)
                }
                Spacer()
                TextField("Search", text: $vm.search)
                    .frame(width: 180)
            }
            .padding()

            if vm.stats.total > 0 {
                HStack(spacing: 12) {
                    Text("\(vm.stats.total) apps")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    if vm.stats.pass > 0 {
                        Text("\(vm.stats.pass) pass")
                            .font(.caption)
                            .foregroundStyle(.green)
                    }
                    if vm.stats.fail > 0 {
                        Text("\(vm.stats.fail) fail")
                            .font(.caption)
                            .foregroundStyle(.red)
                    }
                    Spacer()
                }
                .padding(.horizontal)
            }

            if vm.filteredApps.isEmpty {
                ContentUnavailableView("No manifests", systemImage: "doc.text")
                    .frame(maxHeight: .infinity)
            } else {
                Table(of: ManifestApp.self) {
                    TableColumn("Name") { app in
                        Text(app.name ?? "Untitled")
                    }
                    TableColumn("Path") { app in
                        Text(app.path ?? "—")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                    }
                    TableColumn("Arch") { app in
                        Text(app.arch ?? "auto")
                            .font(.caption)
                    }
                    TableColumn("D3D") { app in
                        Text(app.d3dBackend ?? "none")
                            .font(.caption)
                    }
                    TableColumn("Timeout") { app in
                        Text("\(app.timeout ?? 45)")
                            .font(.caption)
                            .monospacedDigit()
                    }
                    TableColumn("Status") { app in
                        if let status = vm.lastBatchResults[app.id.uuidString] {
                            Image(systemName: status == "PASS" ? "checkmark.circle.fill" : "xmark.circle.fill")
                                .foregroundStyle(status == "PASS" ? .green : .red)
                        } else {
                            Text("—").font(.caption).foregroundStyle(.secondary)
                        }
                    }
                } rows: {
                    ForEach(vm.filteredApps.map { $0.app }) { app in
                        TableRow(app)
                            .contextMenu {
                                Button("Run") { vm.runApp(app) }
                                if let pair = vm.filteredApps.first(where: { $0.app.id == app.id }) {
                                    Button("Export Manifest") { vm.exportManifest(pair.manifest) }
                                    Button("Delete Manifest", role: .destructive) {
                                        vm.deleteManifest(pair.manifest)
                                    }
                                }
                            }
                    }
                }
                .padding(.horizontal)
            }

            if let path = vm.resultsPath {
                Text("Results: \(path)").font(.caption).foregroundStyle(.secondary).padding(.horizontal)
            }
        }
        .sheet(isPresented: $showEditor) {
            if let manifest = editingManifest {
                ManifestEditorView(manifest: manifest, onSave: { _ in vm.refresh(); showEditor = false })
            }
        }
        .onAppear {
            vm.settings = settingsVM.settings
            vm.refresh()
        }
    }
}
