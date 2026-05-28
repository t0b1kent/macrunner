import SwiftUI
import UniformTypeIdentifiers

struct AddAppView: View {
    var app: AppEntry?
    let onSave: (AppEntry) -> Void
    @Environment(\.dismiss) private var dismiss
    @Environment(\.colorScheme) private var scheme

    @State private var name: String = ""
    @State private var exePath: String = ""
    @State private var args: String = ""
    @State private var workdir: String = ""
    @State private var d3dBackend: String = "none"
    @State private var timeout: String = "45"
    @State private var tags: String = ""
    @State private var notes: String = ""
    @State private var pathValid = false
    @State private var dropHovering = false
    @State private var showAdvanced = false

    private var isEditing: Bool { app != nil }

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
        VStack(spacing: 0) {
            header
            Divider().background(Theme.Palette.separator(scheme))
            ScrollView {
                VStack(alignment: .leading, spacing: Theme.Spacing.xl) {
                    dropZone

                    fieldGroup(title: "Identity") {
                        labeledField(label: "Name", placeholder: "e.g. Notepad++") {
                            TextField("", text: $name)
                                .textFieldStyle(.plain)
                                .font(Theme.Font.body)
                                .foregroundColor(Theme.Palette.textPrimary(scheme))
                        }
                        labeledField(label: "Tags", placeholder: "comma separated") {
                            TextField("", text: $tags)
                                .textFieldStyle(.plain)
                                .font(Theme.Font.body)
                                .foregroundColor(Theme.Palette.textPrimary(scheme))
                        }
                    }

                    fieldGroup(title: "Execution") {
                        labeledField(label: "Arguments", placeholder: "optional") {
                            TextField("", text: $args)
                                .textFieldStyle(.plain)
                                .font(Theme.Font.mono)
                                .foregroundColor(Theme.Palette.textPrimary(scheme))
                        }
                        labeledField(label: "Working dir", placeholder: "optional", trailing: {
                            Button("Choose…") { chooseWorkdir() }
                                .buttonStyle(.minimalGhost)
                        }) {
                            TextField("", text: $workdir)
                                .textFieldStyle(.plain)
                                .font(Theme.Font.mono)
                                .foregroundColor(Theme.Palette.textPrimary(scheme))
                        }
                    }

                    DisclosureGroup(isExpanded: $showAdvanced) {
                        VStack(alignment: .leading, spacing: Theme.Spacing.m) {
                            labeledField(label: "D3D backend", placeholder: "") {
                                segmentedPicker(value: $d3dBackend, options: [("None", "none"), ("Mock", "mock"), ("Metal", "metal")])
                            }
                            labeledField(label: "Timeout", placeholder: "seconds") {
                                HStack(spacing: 6) {
                                    TextField("", text: $timeout)
                                        .textFieldStyle(.plain)
                                        .font(Theme.Font.body)
                                        .foregroundColor(Theme.Palette.textPrimary(scheme))
                                        .frame(width: 50, alignment: .leading)
                                    Text("seconds")
                                        .font(Theme.Font.caption)
                                        .foregroundColor(Theme.Palette.textTertiary(scheme))
                                    Spacer()
                                }
                            }
                            VStack(alignment: .leading, spacing: 6) {
                                Text("Notes")
                                    .font(Theme.Font.caption)
                                    .foregroundColor(Theme.Palette.textTertiary(scheme))
                                    .kerning(0.5)
                                TextEditor(text: $notes)
                                    .font(Theme.Font.body)
                                    .foregroundColor(Theme.Palette.textPrimary(scheme))
                                    .scrollContentBackground(.hidden)
                                    .padding(8)
                                    .frame(minHeight: 80)
                                    .background(
                                        RoundedRectangle(cornerRadius: Theme.Radius.small)
                                            .fill(Theme.Palette.bgSecondary(scheme))
                                    )
                                    .overlay(
                                        RoundedRectangle(cornerRadius: Theme.Radius.small)
                                            .stroke(Theme.Palette.border(scheme), lineWidth: 0.5)
                                    )
                            }
                        }
                        .padding(.top, Theme.Spacing.s)
                    } label: {
                        HStack(spacing: 6) {
                            Text("Advanced")
                                .font(Theme.Font.monoCaption)
                                .foregroundColor(Theme.Palette.textTertiary(scheme))
                                .kerning(1)
                        }
                    }
                    .tint(Theme.Palette.textSecondary(scheme))
                }
                .padding(Theme.Spacing.xl)
            }
            Divider().background(Theme.Palette.separator(scheme))
            footer
        }
        .frame(width: 560, height: 640)
        .background(Theme.Palette.bgPrimary(scheme))
    }

    private var header: some View {
        HStack {
            Text(isEditing ? "Edit App" : "Add App")
                .font(Theme.Font.title)
                .foregroundColor(Theme.Palette.textPrimary(scheme))
            Spacer()
            Button { dismiss() } label: {
                Image(systemName: "xmark")
                    .font(.system(size: 11, weight: .medium))
                    .foregroundColor(Theme.Palette.textSecondary(scheme))
                    .frame(width: 28, height: 28)
                    .background(
                        Circle()
                            .stroke(Theme.Palette.border(scheme), lineWidth: 0.5)
                    )
            }
            .buttonStyle(.scalePress)
            .keyboardShortcut(.escape, modifiers: [])
        }
        .padding(Theme.Spacing.xl)
    }

    private var footer: some View {
        HStack {
            if !exePath.isEmpty {
                HStack(spacing: 6) {
                    Image(systemName: pathValid ? "checkmark" : "exclamationmark")
                        .font(.system(size: 10, weight: .bold))
                    Text(pathValid ? "File found" : "Path doesn't exist")
                        .font(Theme.Font.caption)
                }
                .foregroundColor(pathValid ? Theme.Palette.textSecondary(scheme) : Theme.Palette.textPrimary(scheme))
            }
            Spacer()
            Button("Cancel") { dismiss() }
                .buttonStyle(.minimalGhost)
            Button(isEditing ? "Save Changes" : "Add to Library") { save() }
                .buttonStyle(.minimalPrimary)
                .disabled(name.isEmpty || exePath.isEmpty)
                .keyboardShortcut(.return, modifiers: .command)
        }
        .padding(Theme.Spacing.l)
    }

    private var dropZone: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("EXECUTABLE")
                .font(Theme.Font.monoCaption)
                .foregroundColor(Theme.Palette.textTertiary(scheme))
                .kerning(1)
            ZStack {
                RoundedRectangle(cornerRadius: Theme.Radius.medium, style: .continuous)
                    .fill(dropHovering ? Theme.Palette.bgTertiary(scheme) : Theme.Palette.bgSecondary(scheme))
                    .overlay(
                        RoundedRectangle(cornerRadius: Theme.Radius.medium, style: .continuous)
                            .strokeBorder(
                                style: StrokeStyle(
                                    lineWidth: dropHovering ? 1.5 : 0.5,
                                    dash: exePath.isEmpty ? [6, 4] : []
                                )
                            )
                            .foregroundColor(
                                dropHovering
                                    ? Theme.Palette.emphasis(scheme)
                                    : Theme.Palette.border(scheme)
                            )
                    )

                if exePath.isEmpty {
                    VStack(spacing: 6) {
                        Image(systemName: "arrow.down.doc")
                            .font(.system(size: 22, weight: .light))
                            .foregroundColor(Theme.Palette.textTertiary(scheme))
                        Text("Drop a .exe file here")
                            .font(Theme.Font.body)
                            .foregroundColor(Theme.Palette.textSecondary(scheme))
                        Button("or choose file…") { chooseExe() }
                            .buttonStyle(.minimalGhost)
                    }
                    .padding(Theme.Spacing.l)
                } else {
                    HStack(spacing: Theme.Spacing.m) {
                        Image(systemName: "doc.text")
                            .font(.system(size: 18, weight: .light))
                            .foregroundColor(Theme.Palette.textSecondary(scheme))
                        VStack(alignment: .leading, spacing: 2) {
                            Text((exePath as NSString).lastPathComponent)
                                .font(Theme.Font.bodyEmph)
                                .foregroundColor(Theme.Palette.textPrimary(scheme))
                                .lineLimit(1)
                            Text(exePath)
                                .font(Theme.Font.monoCaption)
                                .foregroundColor(Theme.Palette.textTertiary(scheme))
                                .lineLimit(1)
                                .truncationMode(.middle)
                        }
                        Spacer()
                        Button("Replace") { chooseExe() }
                            .buttonStyle(.minimalGhost)
                        Button {
                            exePath = ""
                            pathValid = false
                        } label: {
                            Image(systemName: "xmark")
                                .font(.system(size: 10, weight: .medium))
                                .foregroundColor(Theme.Palette.textSecondary(scheme))
                                .frame(width: 22, height: 22)
                                .background(
                                    Circle().stroke(Theme.Palette.border(scheme), lineWidth: 0.5)
                                )
                        }
                        .buttonStyle(.scalePress)
                    }
                    .padding(Theme.Spacing.m)
                }
            }
            .frame(minHeight: exePath.isEmpty ? 140 : 64)
            .onDrop(of: [.fileURL], isTargeted: $dropHovering) { providers in
                handleDrop(providers)
            }
            .animation(Theme.Motion.standard, value: exePath.isEmpty)
            .animation(Theme.Motion.fast, value: dropHovering)
        }
    }

    private func fieldGroup<Content: View>(
        title: String,
        @ViewBuilder content: () -> Content
    ) -> some View {
        VStack(alignment: .leading, spacing: Theme.Spacing.m) {
            Text(title.uppercased())
                .font(Theme.Font.monoCaption)
                .foregroundColor(Theme.Palette.textTertiary(scheme))
                .kerning(1)
            VStack(alignment: .leading, spacing: Theme.Spacing.m) {
                content()
            }
        }
    }

    private func labeledField<Field: View, Trailing: View>(
        label: String,
        placeholder: String,
        @ViewBuilder trailing: () -> Trailing,
        @ViewBuilder _ content: () -> Field
    ) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text(label)
                    .font(Theme.Font.caption)
                    .foregroundColor(Theme.Palette.textTertiary(scheme))
                    .kerning(0.5)
                Spacer()
                trailing()
            }
            ZStack(alignment: .leading) {
                if placeholderShown(label: label, placeholder: placeholder) {
                    Text(placeholder)
                        .font(Theme.Font.body)
                        .foregroundColor(Theme.Palette.textTertiary(scheme).opacity(0.6))
                        .allowsHitTesting(false)
                }
                content()
            }
            .padding(.vertical, 8)
            .padding(.horizontal, 12)
            .background(
                RoundedRectangle(cornerRadius: Theme.Radius.small)
                    .fill(Theme.Palette.bgSecondary(scheme))
            )
            .overlay(
                RoundedRectangle(cornerRadius: Theme.Radius.small)
                    .stroke(Theme.Palette.border(scheme), lineWidth: 0.5)
            )
        }
    }

    private func labeledField<Field: View>(
        label: String,
        placeholder: String,
        @ViewBuilder _ content: () -> Field
    ) -> some View {
        labeledField(label: label, placeholder: placeholder, trailing: { EmptyView() }, content)
    }

    private func placeholderShown(label: String, placeholder: String) -> Bool {
        switch label {
        case "Name": return name.isEmpty
        case "Tags": return tags.isEmpty
        case "Arguments": return args.isEmpty
        case "Working dir": return workdir.isEmpty
        case "Timeout": return timeout.isEmpty
        default: return false
        }
    }

    private func segmentedPicker(value: Binding<String>, options: [(String, String)]) -> some View {
        HStack(spacing: 0) {
            ForEach(options, id: \.1) { option in
                Button {
                    value.wrappedValue = option.1
                } label: {
                    Text(option.0)
                        .font(Theme.Font.caption)
                        .foregroundColor(
                            value.wrappedValue == option.1
                                ? Theme.Palette.onEmphasis(scheme)
                                : Theme.Palette.textSecondary(scheme)
                        )
                        .padding(.vertical, 6)
                        .frame(maxWidth: .infinity)
                        .background(
                            value.wrappedValue == option.1
                                ? Theme.Palette.emphasis(scheme)
                                : Color.clear
                        )
                }
                .buttonStyle(.plain)
            }
        }
        .overlay(
            RoundedRectangle(cornerRadius: Theme.Radius.small)
                .stroke(Theme.Palette.border(scheme), lineWidth: 0.5)
        )
        .clipShape(RoundedRectangle(cornerRadius: Theme.Radius.small))
    }

    private func handleDrop(_ providers: [NSItemProvider]) -> Bool {
        guard let provider = providers.first else { return false }
        _ = provider.loadObject(ofClass: URL.self) { url, _ in
            guard let url = url else { return }
            DispatchQueue.main.async {
                accept(path: url.path)
            }
        }
        return true
    }

    private func chooseExe() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        if let exe = UTType(filenameExtension: "exe") {
            panel.allowedContentTypes = [exe]
        }
        if panel.runModal() == .OK, let url = panel.url {
            accept(path: url.path)
        }
    }

    private func chooseWorkdir() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        if panel.runModal() == .OK, let url = panel.url {
            workdir = url.path
        }
    }

    private func accept(path: String) {
        exePath = path
        pathValid = FileManager.default.fileExists(atPath: path)
        if name.isEmpty {
            let basename = (path as NSString).lastPathComponent
            name = (basename as NSString).deletingPathExtension
        }
    }

    private func save() {
        var entry = app ?? AppEntry.new(name: name, exePath: exePath)
        entry.name = name
        entry.exePath = exePath
        entry.args = args.split(separator: " ").map(String.init)
        entry.workdir = workdir.isEmpty ? nil : workdir
        entry.d3dBackend = d3dBackend
        entry.timeout = Int(timeout) ?? 45
        entry.tags = tags
            .split(separator: ",")
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty }
        entry.notes = notes.isEmpty ? nil : notes
        onSave(entry)
    }
}
