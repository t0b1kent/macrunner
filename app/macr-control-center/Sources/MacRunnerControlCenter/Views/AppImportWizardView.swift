import SwiftUI
import UniformTypeIdentifiers

struct AppImportWizardView: View {
    @EnvironmentObject var settingsVM: SettingsViewModel
    @Environment(\.dismiss) private var dismiss
    @State private var step = 0
    @State private var exePath = ""
    @State private var isFolderScan = false
    @State private var scannedExes: [String] = []
    @State private var selectedScanned: String?
    @State private var inspection: PEInspectionResult?
    @State private var isInspecting = false
    @State private var errorText: String?
    @State private var appName = ""
    @State private var d3dBackend = "none"
    @State private var timeout = "45"
    @State private var workdir = ""
    @State private var args = ""
    @State private var tags = ""
    @State private var notes = ""
    @State private var knownEntry: CompatibilityEntry?
    @State private var isDuplicate = false
    @State private var createManifest = false

    private let steps = ["Select", "Inspect", "Review"]

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text("Import Windows App").font(.title)
                Spacer()
            }
            .padding()

            StepBar(steps: steps, current: step)
                .padding(.horizontal)

            if step == 0 {
                dropZone
            } else if step == 1 {
                inspectionView
            } else if step == 2 {
                reviewView
            }

            HStack {
                if step > 0 {
                    Button("Back") { step -= 1 }
                }
                Spacer()
                if step == 0 {
                    if !scannedExes.isEmpty {
                        Button("Next") {
                            if let selected = selectedScanned {
                                exePath = selected
                                performInspection()
                            }
                        }
                        .disabled(selectedScanned == nil)
                    } else {
                        Button("Next") {
                            if !exePath.isEmpty {
                                performInspection()
                            }
                        }
                        .disabled(exePath.isEmpty)
                    }
                } else if step == 1 {
                    Button("Next") { step = 2 }
                } else if step == 2 {
                    Button("Save to Library") { save() }
                }
            }
            .padding()
        }
        .frame(minWidth: 600, minHeight: 500)
    }

    var dropZone: some View {
        VStack(spacing: 16) {
            Picker("Import type", selection: $isFolderScan) {
                Text("Single .exe").tag(false)
                Text("Folder scan").tag(true)
            }
            .pickerStyle(.segmented)
            .padding(.horizontal)

            if isFolderScan {
                HStack {
                    TextField("Folder path", text: $exePath)
                    Button("Choose Folder...") {
                        let panel = NSOpenPanel()
                        panel.canChooseDirectories = true
                        panel.canChooseFiles = false
                        if panel.runModal() == .OK {
                            exePath = panel.url?.path ?? ""
                            scanFolder()
                        }
                    }
                }

                if !scannedExes.isEmpty {
                    Text("Found \(scannedExes.count) .exe file(s)")
                        .font(.caption)
                        .foregroundStyle(.secondary)

                    List(scannedExes, id: \.self, selection: $selectedScanned) { path in
                        HStack {
                            Text((path as NSString).lastPathComponent)
                            Spacer()
                            Text(path)
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                                .lineLimit(1)
                        }
                    }
                    .frame(minHeight: 150)
                }
            } else {
                TextField("Path to .exe", text: $exePath)
                Button("Choose File...") {
                    let panel = NSOpenPanel()
                    panel.allowedContentTypes = [UTType(filenameExtension: "exe")!]
                    if panel.runModal() == .OK {
                        exePath = panel.url?.path ?? ""
                    }
                }
            }

            if !exePath.isEmpty {
                Text(exePath).font(.caption).foregroundStyle(.secondary)
                if !isPathSafe() {
                    Label("Path is outside MacRunner root. Proceed with caution.", systemImage: "exclamationmark.triangle")
                        .foregroundStyle(.orange)
                        .font(.caption)
                }
            }
            if let error = errorText {
                Text(error).foregroundStyle(.red)
            }
        }
        .padding()
        .onDrop(of: [.fileURL], isTargeted: nil) { providers in
            guard let provider = providers.first else { return false }
            _ = provider.loadObject(ofClass: URL.self) { url, _ in
                if let url = url {
                    DispatchQueue.main.async {
                        if url.pathExtension.lowercased() == "exe" {
                            self.exePath = url.path
                            self.isFolderScan = false
                        } else {
                            var isDir: ObjCBool = false
                            if FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir), isDir.boolValue {
                                self.exePath = url.path
                                self.isFolderScan = true
                                self.scanFolder()
                            }
                        }
                    }
                }
            }
            return true
        }
    }

    var inspectionView: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                if isInspecting {
                    ProgressView("Inspecting...")
                        .frame(maxWidth: .infinity, alignment: .center)
                        .padding()
                } else if let result = inspection {
                    if isDuplicate {
                        Label("Duplicate: this .exe is already in your library", systemImage: "exclamationmark.triangle.fill")
                            .foregroundStyle(.orange)
                            .font(.callout)
                    }

                    if let known = knownEntry {
                        HStack {
                            Image(systemName: "checkmark.seal.fill")
                                .foregroundStyle(.blue)
                            Text("Known in Compatibility DB: \(known.lastStatus ?? "unknown")")
                                .font(.callout)
                        }
                        .padding(.vertical, 4)
                    }

                    if let category = result.category {
                        HStack {
                            Image(systemName: categoryIcon(category))
                                .foregroundStyle(Color.accentColor)
                            Text("Category: \(category.rawValue)")
                                .font(.callout.bold())
                            Spacer()
                        }
                    }

                    WizardInfoRow(label: "Architecture", value: result.arch)
                    WizardInfoRow(label: "Lane", value: result.suggestedLane ?? "auto")
                    WizardInfoRow(label: "Subsystem", value: result.subsystem)
                    WizardInfoRow(label: "64-bit", value: result.is64Bit.map { $0 ? "Yes" : "No" } ?? "—")
                    WizardInfoRow(label: ".NET", value: result.isDotNet.map { $0 ? "Yes" : "No" } ?? "—")
                    WizardInfoRow(label: "D3D detected", value: result.d3dDetected ? "Yes" : "No")
                    if let d3dv = result.d3dVersion {
                        WizardInfoRow(label: "D3D version", value: d3dv)
                    }
                    if let gfx = result.graphicsAPI {
                        WizardInfoRow(label: "Graphics API", value: gfx)
                    }
                    if let audio = result.audioAPI {
                        WizardInfoRow(label: "Audio API", value: audio)
                    }
                    if let input = result.inputAPI {
                        WizardInfoRow(label: "Input API", value: input)
                    }
                    WizardInfoRow(label: "Suggested backend", value: result.suggestedD3DBackend)
                    WizardInfoRow(label: "Suggested timeout", value: "\(result.suggestedTimeout)s")
                    if let size = result.fileSizeBytes {
                        WizardInfoRow(label: "File size", value: "\(size) bytes")
                    }

                    if let deps = result.dllDependencies, !deps.isEmpty {
                        DisclosureGroup("DLL Dependencies (\(deps.count))") {
                            VStack(alignment: .leading, spacing: 2) {
                                ForEach(deps, id: \.self) { dep in
                                    Text(dep).font(.caption).foregroundStyle(.secondary)
                                }
                            }
                            .padding(.leading, 8)
                        }
                    }

                    if let imports = result.imports, !imports.isEmpty {
                        DisclosureGroup("Imports (\(imports.count))") {
                            VStack(alignment: .leading, spacing: 2) {
                                ForEach(imports.prefix(50), id: \.self) { imp in
                                    Text(imp).font(.caption).foregroundStyle(.secondary)
                                }
                                if imports.count > 50 {
                                    Text("... and \(imports.count - 50) more").font(.caption).foregroundStyle(.secondary)
                                }
                            }
                            .padding(.leading, 8)
                        }
                    }

                    if let sections = result.sections, !sections.isEmpty {
                        DisclosureGroup("Sections (\(sections.count))") {
                            LazyVGrid(columns: [GridItem(.adaptive(minimum: 80))], alignment: .leading) {
                                ForEach(sections, id: \.self) { section in
                                    Text(section)
                                        .font(.caption)
                                        .padding(.horizontal, 6)
                                        .padding(.vertical, 2)
                                        .background(Color.secondary.opacity(0.15))
                                        .cornerRadius(4)
                                }
                            }
                            .padding(.leading, 8)
                        }
                    }
                } else {
                    Text("Inspection failed or unavailable.").foregroundStyle(.secondary)
                }
            }
            .padding()
        }
    }

    var reviewView: some View {
        Form {
            Section(header: Text("Basic")) {
                TextField("Name", text: $appName)
                TextField("Args", text: $args)
                TextField("Workdir", text: $workdir)
            }
            Section(header: Text("Settings")) {
                Picker("D3D Backend", selection: $d3dBackend) {
                    Text("None").tag("none")
                    Text("Mock").tag("mock")
                    Text("Metal").tag("metal")
                }
                TextField("Timeout", text: $timeout)
            }
            Section(header: Text("Metadata")) {
                TextField("Tags (comma separated)", text: $tags)
                TextEditor(text: $notes)
                    .frame(minHeight: 60)
            }
            Section(header: Text("Corpus")) {
                Toggle("Create corpus manifest", isOn: $createManifest)
            }
        }
        .formStyle(.grouped)
        .padding()
    }

    private func isPathSafe() -> Bool {
        let root = settingsVM.settings.macRunnerRoot
        return PathSafety.isSubpath(of: root, path: exePath)
    }

    private func scanFolder() {
        scannedExes = PEInspectionService.scanFolder(exePath)
        selectedScanned = scannedExes.first
    }

    private func performInspection() {
        isInspecting = true
        errorText = nil
        Task {
            let result = await PEInspectionService.inspect(
                exePath: exePath,
                macRunnerRoot: settingsVM.settings.macRunnerRoot
            )
            await MainActor.run {
                self.isInspecting = false
                let existingApps = ConfigStore.shared.loadApps()
                self.isDuplicate = PEInspectionService.isDuplicate(exePath: exePath, apps: existingApps)

                if let result = result {
                    self.inspection = result
                    self.d3dBackend = result.suggestedD3DBackend
                    self.timeout = "\(result.suggestedTimeout)"
                    self.appName = (exePath as NSString).lastPathComponent
                    self.workdir = (exePath as NSString).deletingLastPathComponent
                    self.tags = result.category?.rawValue ?? ""
                    self.step = 1
                } else {
                    self.errorText = "Could not inspect. Proceeding with defaults."
                    self.appName = (exePath as NSString).lastPathComponent
                    self.workdir = (exePath as NSString).deletingLastPathComponent
                    self.step = 1
                }
                self.checkCompatibilityDB()
            }
        }
    }

    private func checkCompatibilityDB() {
        let entries = CompatibilityStore.shared.loadEntries()
        let id = exePath
        knownEntry = entries.first { $0.exePathHash == id }
    }

    private func save() {
        var entry = AppEntry.new(name: appName, exePath: exePath)
        entry.d3dBackend = d3dBackend
        entry.timeout = Int(timeout) ?? 45
        entry.workdir = workdir.isEmpty ? nil : workdir
        entry.args = args.split(separator: " ").map(String.init)
        entry.tags = tags.isEmpty ? nil : tags.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }
        entry.notes = notes.isEmpty ? nil : notes
        if let result = inspection {
            entry.arch = result.arch
        }
        ConfigStore.shared.saveApps(ConfigStore.shared.loadApps() + [entry])

        if createManifest, let result = inspection {
            let manifestApp = PEInspectionService.createManifestEntry(from: result, exePath: exePath, name: appName)
            let manifest = RealAppManifest(schemaVersion: 1, apps: [manifestApp])
            let dir = "\(settingsVM.settings.macRunnerRoot)/tests/real-app-manifests"
            let path = "\(dir)/\(entry.id.uuidString).json"
            try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
            if let data = try? JSONEncoder().encode(manifest) {
                try? data.write(to: URL(fileURLWithPath: path))
            }
        }

        dismiss()
    }

    private func categoryIcon(_ category: AppCategory) -> String {
        switch category {
        case .console: return "terminal"
        case .gui: return "window"
        case .gdi: return "paintbrush"
        case .d3d11, .d3d12: return "cube"
        case .installer: return "archivebox"
        case .gameDemo: return "gamecontroller"
        default: return "questionmark"
        }
    }
}

private struct WizardInfoRow: View {
    let label: String
    let value: String

    var body: some View {
        HStack {
            Text(label)
                .font(.callout)
                .foregroundStyle(.secondary)
            Spacer()
            Text(value)
                .font(.callout)
                .textSelection(.enabled)
        }
    }
}
