import SwiftUI

struct ReleaseManagerView: View {
    @EnvironmentObject var settingsVM: SettingsViewModel
    @StateObject private var vm: ReleaseManagerViewModel

    init() {
        self._vm = StateObject(wrappedValue: ReleaseManagerViewModel(settings: SettingsViewModel().settings))
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Text("Release Manager")
                    .font(.headline)
                Spacer()
                Button("Export Release") { vm.exportRelease() }
                    .disabled(vm.isExporting)
                if vm.isExporting {
                    ProgressView("Exporting...")
                        .padding(.leading, 8)
                }
            }
            .padding()

            if let path = vm.lastExportPath {
                HStack {
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundStyle(.green)
                    Text("Created:")
                        .font(.caption)
                    Text(URL(fileURLWithPath: path).lastPathComponent)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Button("Reveal") {
                        NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: path)])
                    }
                    Spacer()
                }
                .padding(.horizontal)
            }

            if let error = vm.exportError {
                HStack {
                    Image(systemName: "xmark.circle.fill")
                        .foregroundStyle(.red)
                    Text(error)
                        .font(.caption)
                        .foregroundStyle(.red)
                    Spacer()
                }
                .padding(.horizontal)
            }

            Form {
                Section("Release Contents") {
                    Toggle("Compatibility DB", isOn: $vm.includeCompatibilityDB)
                    Toggle("Performance Report", isOn: $vm.includePerformanceReport)
                    Toggle("Manifests", isOn: $vm.includeManifests)
                    Toggle("Debug Bundles", isOn: $vm.includeDebugBundles)
                }
                Section("Release Notes") {
                    TextEditor(text: $vm.releaseNotes)
                        .frame(minHeight: 80)
                }
            }
            .formStyle(.grouped)
            .padding(.horizontal)

            if vm.releases.isEmpty {
                ContentUnavailableView("No releases yet", systemImage: "archivebox.fill")
                    .frame(maxHeight: .infinity)
            } else {
                Text("Previous Releases")
                    .font(.headline)
                    .padding(.horizontal)

                List(vm.releases) { release in
                    HStack {
                        Image(systemName: "doc.zipper")
                            .foregroundStyle(.secondary)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(release.name)
                                .font(.callout)
                            Text(Formatters.byteCountString(release.sizeBytes))
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                            Text(Formatters.mediumDate.string(from: release.createdAt))
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                        }
                        Spacer()
                        Button("Reveal") { vm.revealInFinder(release) }
                        Button("Delete", role: .destructive) { vm.deleteRelease(release) }
                    }
                }
                .listStyle(.inset)
            }
        }
        .onAppear {
            vm.settings = settingsVM.settings
            vm.scanReleases()
        }
    }
}
