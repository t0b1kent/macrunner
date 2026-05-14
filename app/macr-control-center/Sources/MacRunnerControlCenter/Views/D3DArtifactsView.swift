import SwiftUI

struct D3DArtifactsView: View {
    @StateObject private var vm = D3DArtifactsViewModel()
    @EnvironmentObject var settingsVM: SettingsViewModel
    @State private var resultPath: String = ""
    @State private var comparePath: String = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                TextField("Launcher result JSON path", text: $resultPath)
                Button("Load") { load() }
                Button("Load Latest") { loadLatest() }
                Button("Compare...") { loadCompare() }
                    .disabled(vm.launcherResult == nil)
                if vm.isComparing {
                    Button("End Compare") { vm.isComparing = false }
                }
                Button("Clear") { vm.clear() }
                Spacer()
                Button("Run D3D Smoke") { runD3DSmoke() }
            }
            .padding()

            if let result = vm.launcherResult {
                HStack {
                    StatusBadge(status: result.d3dStatus)
                    Text("Backend: \(result.d3dBackend ?? "—")")
                    if let cs = result.d3dOutputChecksum {
                        Text("Checksum: \(cs.prefix(16))…").font(.caption).foregroundStyle(.secondary)
                    }
                    if let px = result.d3dNonBackgroundPixels {
                        Text("Pixels: \(px)").font(.caption).foregroundStyle(.secondary)
                    }
                    if let uc = result.d3dUnsupportedCalls {
                        Text("Unsupported: \(uc)").font(.caption).foregroundStyle(.red)
                    }
                    Spacer()
                    if !vm.callCounts.isEmpty {
                        Text("Top call: \(vm.callCounts[0].0) (\(vm.callCounts[0].1))")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
                .padding(.horizontal)

                if vm.isComparing, let compare = vm.compareResult {
                    HStack {
                        StatusBadge(status: compare.d3dStatus)
                        Text("Compare: \(compare.d3dBackend ?? "—")")
                        if let cs = compare.d3dOutputChecksum {
                            Text("Checksum: \(cs.prefix(16))…").font(.caption).foregroundStyle(.secondary)
                        }
                        if let px = compare.d3dNonBackgroundPixels {
                            Text("Pixels: \(px)").font(.caption).foregroundStyle(.secondary)
                        }
                        if vm.launcherResult?.d3dOutputChecksum == compare.d3dOutputChecksum {
                            Label("Identical", systemImage: "checkmark.circle.fill")
                                .foregroundStyle(.green)
                                .font(.caption)
                        }
                        Spacer()
                    }
                    .padding(.horizontal)
                }

                HStack {
                    TextField("Filter trace", text: $vm.traceFilter)
                        .frame(width: 200)
                    Text("\(vm.filteredTraceLines.count) / \(vm.traceLines.count) lines")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Spacer()
                }
                .padding(.horizontal)

                TabView {
                    PPMImageView(image: vm.ppmImage)
                        .tabItem { Label("PPM", systemImage: "photo") }
                    if vm.isComparing {
                        PPMImageView(image: vm.comparePpmImage)
                            .tabItem { Label("Compare PPM", systemImage: "photo.on.rectangle") }
                    }
                    TraceViewerView(lines: vm.filteredTraceLines)
                        .tabItem { Label("Trace", systemImage: "list.bullet.indent") }
                    if vm.isComparing {
                        TraceViewerView(lines: vm.compareTraceLines)
                            .tabItem { Label("Compare Trace", systemImage: "list.bullet.indent") }
                    }
                    IRReportView(title: "IR", json: vm.irJSON)
                        .tabItem { Label("IR", systemImage: "doc.text") }
                    IRReportView(title: "Report", json: vm.reportJSON)
                        .tabItem { Label("Report", systemImage: "doc.plaintext") }
                }
                .padding(.horizontal)
            } else {
                ContentUnavailableView("No D3D result loaded", systemImage: "cube.transparent")
                    .frame(maxHeight: .infinity)
            }
        }
        .onAppear {
            if vm.launcherResult == nil {
                loadLatest()
            }
        }
    }

    private func load() {
        guard let data = try? Data(contentsOf: URL(fileURLWithPath: resultPath)) else { return }
        if let result = try? JSONDecoder().decode(LauncherResult.self, from: data) {
            vm.load(from: result)
        }
    }

    private func loadLatest() {
        let dir = "\(settingsVM.settings.macRunnerRoot)/artifacts/control-center"
        let fm = FileManager.default
        guard let subs = try? fm.contentsOfDirectory(atPath: dir) else { return }
        var latest: String?
        for sub in subs {
            let p = "\(dir)/\(sub)/last-run.json"
            if fm.fileExists(atPath: p) {
                latest = p
            }
        }
        if let p = latest {
            resultPath = p
            load()
        }
    }

    private func loadCompare() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.json]
        if panel.runModal() == .OK, let url = panel.url,
           let data = try? Data(contentsOf: url),
           let result = try? JSONDecoder().decode(LauncherResult.self, from: data) {
            comparePath = url.path
            vm.loadCompare(from: result)
            vm.isComparing = true
        }
    }

    private func runD3DSmoke() {
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
