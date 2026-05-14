import SwiftUI

struct ManifestEditorView: View {
    let manifest: RealAppManifest
    let onSave: (RealAppManifest) -> Void
    @Environment(\.dismiss) private var dismiss

    @State private var name: String = ""
    @State private var path: String = ""
    @State private var arch: String = ""
    @State private var args: String = ""
    @State private var env: String = ""
    @State private var workdir: String = ""
    @State private var expectedRc: String = "0"
    @State private var expectedStdout: String = ""
    @State private var expectedFiles: String = ""
    @State private var d3dBackend: String = "none"
    @State private var allowGui: Bool = false
    @State private var timeout: String = "45"

    init(manifest: RealAppManifest, onSave: @escaping (RealAppManifest) -> Void) {
        self.manifest = manifest
        self.onSave = onSave
        if let app = manifest.apps?.first {
            _name = State(initialValue: app.name ?? "")
            _path = State(initialValue: app.path ?? "")
            _arch = State(initialValue: app.arch ?? "")
            _args = State(initialValue: app.args?.joined(separator: " ") ?? "")
            _workdir = State(initialValue: app.workdir ?? "")
            _expectedRc = State(initialValue: "\(app.expectedRc ?? 0)")
            _expectedStdout = State(initialValue: app.expectedStdoutContains ?? "")
            _expectedFiles = State(initialValue: app.expectedFiles?.joined(separator: ", ") ?? "")
            _d3dBackend = State(initialValue: app.d3dBackend ?? "none")
            _allowGui = State(initialValue: app.allowGui ?? false)
            _timeout = State(initialValue: "\(app.timeout ?? 45)")
        }
    }

    var body: some View {
        Form {
            Section("App") {
                TextField("Name", text: $name)
                HStack {
                    TextField("Path to .exe", text: $path)
                    Button("Choose…") {
                        let panel = NSOpenPanel()
                        panel.allowsMultipleSelection = false
                        panel.canChooseDirectories = false
                        if panel.runModal() == .OK {
                            path = panel.url?.path ?? ""
                        }
                    }
                }
                TextField("Arch (arm64/x64/x86)", text: $arch)
                TextField("Args", text: $args)
                TextField("Workdir", text: $workdir)
            }
            Section("Expectations") {
                TextField("Expected RC", text: $expectedRc)
                TextField("Expected stdout contains", text: $expectedStdout)
                TextField("Expected files (comma separated)", text: $expectedFiles)
            }
            Section("Execution") {
                Picker("D3D Backend", selection: $d3dBackend) {
                    Text("None").tag("none")
                    Text("Mock").tag("mock")
                    Text("Metal").tag("metal")
                }
                Toggle("Allow GUI", isOn: $allowGui)
                TextField("Timeout", text: $timeout)
            }
        }
        .formStyle(.grouped)
        .padding()
        .frame(minWidth: 480, minHeight: 420)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) {
                Button("Cancel") { dismiss() }
            }
            ToolbarItem(placement: .confirmationAction) {
                Button("Save") {
                    let app = ManifestApp(
                        name: name,
                        path: path,
                        arch: arch.isEmpty ? nil : arch,
                        args: args.split(separator: " ").map(String.init),
                        env: nil,
                        workdir: workdir.isEmpty ? nil : workdir,
                        expectedRc: Int(expectedRc),
                        expectedStdoutContains: expectedStdout.isEmpty ? nil : expectedStdout,
                        expectedFiles: expectedFiles.isEmpty ? nil : expectedFiles.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) },
                        d3dBackend: d3dBackend,
                        allowGui: allowGui,
                        timeout: Int(timeout) ?? 45
                    )
                    let manifest = RealAppManifest(schemaVersion: 1, apps: [app])
                    onSave(manifest)
                }
                .disabled(name.isEmpty || path.isEmpty)
            }
        }
    }
}
