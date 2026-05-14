import SwiftUI

struct DebugBundleView: View {
    @EnvironmentObject var settingsVM: SettingsViewModel
    @StateObject private var vm: DebugBundleViewModel
    @State private var showOptions = false

    init() {
        self._vm = StateObject(wrappedValue: DebugBundleViewModel(settings: SettingsViewModel().settings))
    }

    var body: some View {
        VStack {
            HStack {
                Button("Export Bundle") {
                    vm.export(settings: settingsVM.settings)
                }
                .disabled(vm.isExporting)
                Button("Options") { showOptions = true }
                Button("Reset Options") { vm.resetOptions() }
                if vm.isExporting {
                    ProgressView("Exporting...")
                        .padding(.leading, 8)
                }
                Spacer()
            }
            .padding()

            if let url = vm.lastBundleURL {
                HStack {
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundStyle(.green)
                    Text("Saved to Desktop:")
                        .font(.caption)
                    Text(url.lastPathComponent)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Button("Reveal") { vm.revealInFinder(url) }
                    Spacer()
                }
                .padding(.horizontal)
            }

            if !vm.recentBundles.isEmpty {
                Text("Recent Bundles")
                    .font(.headline)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal)

                List(vm.recentBundles, id: \.absoluteString) { url in
                    HStack {
                        Image(systemName: "doc.zipper")
                            .foregroundStyle(.secondary)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(url.lastPathComponent)
                                .font(.callout)
                            if let attrs = try? FileManager.default.attributesOfItem(atPath: url.path),
                               let mod = attrs[.modificationDate] as? Date {
                                Text(Formatters.mediumDate.string(from: mod))
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                            }
                        }
                        Spacer()
                        Button("Reveal") { vm.revealInFinder(url) }
                        Button("Remove") { vm.removeFromRecent(url) }
                    }
                }
                .listStyle(.inset)
            } else {
                ContentUnavailableView("No bundles yet", systemImage: "doc.zipper")
                    .frame(maxHeight: .infinity)
            }
        }
        .sheet(isPresented: $showOptions) {
            BundleOptionsSheet(options: $vm.options)
        }
        .onAppear {
            vm.settings = settingsVM.settings
        }
    }
}

private struct BundleOptionsSheet: View {
    @Binding var options: DebugBundleExporterV2.Options
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack {
                Text("Bundle Options")
                    .font(.title2)
                Spacer()
                Button("Done") { dismiss() }
            }

            Form {
                Toggle("Include Artifacts", isOn: $options.includeArtifacts)
                Toggle("Include Logs", isOn: $options.includeLogs)
                Toggle("Include Reports", isOn: $options.includeReports)
                Toggle("Include Compatibility", isOn: $options.includeCompatibility)
                Toggle("Include Manifest", isOn: $options.includeManifest)
                Toggle("Include Process Snapshot", isOn: $options.includeProcessSnapshot)
                Toggle("Include System Info", isOn: $options.includeSystemInfo)
                Toggle("Include Git Diff", isOn: $options.includeGitDiff)
                HStack {
                    Text("Max File Size")
                    Spacer()
                    TextField("Bytes", value: $options.maxFileSizeBytes, format: .number)
                        .frame(width: 100)
                        .textFieldStyle(.roundedBorder)
                }
            }
            .formStyle(.grouped)
        }
        .padding()
        .frame(minWidth: 400, minHeight: 400)
    }
}
