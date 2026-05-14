import SwiftUI

struct AddAppView: View {
    var app: AppEntry?
    let onSave: (AppEntry) -> Void
    @Environment(\.dismiss) private var dismiss

    @State private var name: String = ""
    @State private var exePath: String = ""
    @State private var args: String = ""
    @State private var workdir: String = ""
    @State private var d3dBackend: String = "none"
    @State private var timeout: String = "45"
    @State private var tags: String = ""
    @State private var notes: String = ""
    @State private var pathValid = false

    init(app: AppEntry? = nil, onSave: @escaping (AppEntry) -> Void) {
        self.app = app
        self.onSave = onSave
        if let app = app {
            _name = State(initialValue: app.name)
            _exePath = State(initialValue: app.exePath)
            _args = State(initialValue: app.args?.joined(separator: " ") ?? "")
            _workdir = State(initialValue: app.workdir ?? "")
            _d3dBackend = State(initialValue: app.d3dBackend)
            _timeout = State(initialValue: "\(app.timeout ?? 45)")
            _tags = State(initialValue: app.tags?.joined(separator: ", ") ?? "")
            _notes = State(initialValue: app.notes ?? "")
        }
    }

    var body: some View {
        Form {
            Section("Executable") {
                TextField("Name", text: $name)
                HStack {
                    TextField(".exe path", text: $exePath)
                        .onChange(of: exePath) { validatePath() }
                    Image(systemName: pathValid ? "checkmark.circle.fill" : "xmark.circle.fill")
                        .foregroundStyle(pathValid ? .green : .red)
                }
                TextField("Arguments", text: $args)
                TextField("Working Directory", text: $workdir)
            }
            Section("Execution") {
                Picker("D3D Backend", selection: $d3dBackend) {
                    Text("None").tag("none")
                    Text("Mock").tag("mock")
                    Text("Metal").tag("metal")
                }
                TextField("Timeout (seconds)", text: $timeout)
            }
            Section("Metadata") {
                TextField("Tags (comma separated)", text: $tags)
                TextField("Notes", text: $notes, axis: .vertical)
                    .lineLimit(3...6)
            }
        }
        .formStyle(.grouped)
        .padding()
        .frame(minWidth: 480, minHeight: 360)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) {
                Button("Cancel") { dismiss() }
            }
            ToolbarItem(placement: .confirmationAction) {
                Button("Save") {
                    var entry = app ?? AppEntry.new(name: name, exePath: exePath)
                    entry.name = name
                    entry.exePath = exePath
                    entry.args = args.split(separator: " ").map(String.init)
                    entry.workdir = workdir.isEmpty ? nil : workdir
                    entry.d3dBackend = d3dBackend
                    entry.timeout = Int(timeout) ?? 45
                    entry.tags = tags.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }
                    entry.notes = notes.isEmpty ? nil : notes
                    onSave(entry)
                }
                .disabled(name.isEmpty || exePath.isEmpty)
            }
        }
    }

    private func validatePath() {
        pathValid = FileManager.default.fileExists(atPath: exePath)
    }
}
