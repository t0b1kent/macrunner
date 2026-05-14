import SwiftUI

struct CompatibilityDBView: View {
    @EnvironmentObject var settingsVM: SettingsViewModel
    @StateObject private var vm = CompatibilityDBViewModel()
    @State private var showDetail = false
    @State private var selection: String?

    var body: some View {
        VStack {
            HStack {
                TextField("Search", text: $vm.search).frame(width: 180)
                Picker("Status", selection: $vm.filterStatus) {
                    Text("Any").tag(nil as CompatibilityStatus?)
                    ForEach(CompatibilityStatus.allCases, id: \.self) { s in
                        Text(s.rawValue).tag(s as CompatibilityStatus?)
                    }
                }
                .frame(width: 140)
                Picker("Category", selection: $vm.filterCategory) {
                    Text("Any").tag(nil as AppCategory?)
                    ForEach([AppCategory.console, .gui, .gdi, .d3d11, .d3d12, .installer, .gameDemo], id: \.self) { c in
                        Text(c.rawValue).tag(c as AppCategory?)
                    }
                }
                .frame(width: 140)
                TextField("Arch", text: $vm.filterArch).frame(width: 80)
                TextField("D3D", text: $vm.filterD3D).frame(width: 80)
                Button("Export JSON") { vm.exportJSON() }
                Button("Import JSON") { vm.importJSON() }
                Button("Regressions Manifest") {
                    if let path = vm.generateRegressionManifest(macRunnerRoot: settingsVM.settings.macRunnerRoot) {
                        NSWorkspace.shared.open(URL(fileURLWithPath: path))
                    }
                }
                Button("Copy Corpus Prompt") {
                    let prompt = CodexPromptService.generateCorpusPrompt(entries: vm.filtered)
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(prompt, forType: .string)
                }
                Button("Refresh") { vm.refresh() }
                Spacer()
            }
            .padding()

            Table(of: CompatibilityEntry.self, selection: $selection) {
                TableColumn("Name") { e in
                    HStack {
                        if vm.isRegression(e) {
                            Image(systemName: "exclamationmark.triangle.fill")
                                .foregroundStyle(.red)
                                .help("Regression detected")
                        }
                        Text(e.name)
                    }
                }
                TableColumn("Arch") { e in Text(e.arch ?? "?") }
                TableColumn("Status") { e in StatusBadge(status: e.lastStatus) }
                TableColumn("D3D") { e in Text(e.bestD3DBackend ?? "—") }
                TableColumn("Suggested") { e in
                    if let suggested = vm.suggestedBackend(for: e), suggested != e.bestD3DBackend {
                        Text(suggested).foregroundStyle(.blue)
                    } else {
                        Text("—")
                    }
                }
                TableColumn("Pass Rate") { e in
                    let rate = vm.passRate(for: e)
                    Text("\(Int(rate * 100))%")
                        .foregroundStyle(rate >= 0.8 ? .green : rate >= 0.5 ? .orange : .red)
                }
                TableColumn("Category") { e in Text(e.category?.rawValue ?? "—") }
                TableColumn("Fails") { e in Text("\(e.failuresCount)") }
                TableColumn("Tags") { e in Text(e.tags?.joined(separator: ", ") ?? "—") }
                TableColumn("Last Run") { e in
                    if let d = e.lastRunDate {
                        Text(Formatters.mediumDate.string(from: d))
                    } else { Text("—") }
                }
            } rows: {
                ForEach(vm.filtered) { e in TableRow(e) }
            }
            .padding(.horizontal)
            .onChange(of: selection) { _, new in
                if let id = new, let entry = vm.filtered.first(where: { $0.id == id }) {
                    vm.selectedEntry = entry
                    showDetail = true
                }
            }
        }
        .sheet(isPresented: $showDetail) {
            if let entry = vm.selectedEntry {
                CompatibilityDetailSheet(entry: entry, vm: vm)
            }
        }
        .onAppear { vm.refresh() }
    }
}

struct CompatibilityDetailSheet: View {
    @Environment(\.dismiss) private var dismiss
    @State var entry: CompatibilityEntry
    @ObservedObject var vm: CompatibilityDBViewModel

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack {
                Text("Compatibility Entry")
                    .font(.title2)
                Spacer()
                Button("Done") { dismiss() }
            }

            Form {
                Section(header: Text("Identity")) {
                    TextField("Name", text: $entry.name)
                    TextField("Arch", text: Binding(
                        get: { entry.arch ?? "" },
                        set: { entry.arch = $0.isEmpty ? nil : $0 }
                    ))
                }

                Section(header: Text("Status & Backend")) {
                    TextField("Last Status", text: Binding(
                        get: { entry.lastStatus ?? "" },
                        set: { entry.lastStatus = $0.isEmpty ? nil : $0 }
                    ))
                    TextField("Best D3D Backend", text: Binding(
                        get: { entry.bestD3DBackend ?? "" },
                        set: { entry.bestD3DBackend = $0.isEmpty ? nil : $0 }
                    ))
                    TextField("Last Successful Version", text: Binding(
                        get: { entry.lastSuccessfulVersion ?? "" },
                        set: { entry.lastSuccessfulVersion = $0.isEmpty ? nil : $0 }
                    ))
                }

                Section(header: Text("Classification")) {
                    Picker("Category", selection: $entry.category) {
                        Text("None").tag(nil as AppCategory?)
                        ForEach([AppCategory.console, .gui, .gdi, .d3d11, .d3d12, .installer, .gameDemo], id: \.self) { c in
                            Text(c.rawValue).tag(c as AppCategory?)
                        }
                    }
                    TextField("Tags (comma separated)", text: Binding(
                        get: { entry.tags?.joined(separator: ", ") ?? "" },
                        set: { entry.tags = $0.isEmpty ? nil : $0.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) } }
                    ))
                }

                Section(header: Text("Notes")) {
                    TextEditor(text: Binding(
                        get: { entry.notes ?? "" },
                        set: { entry.notes = $0.isEmpty ? nil : $0 }
                    ))
                    .frame(minHeight: 80)
                }
            }

            HStack {
                Button("Delete", role: .destructive) {
                    vm.deleteEntry(entry)
                    dismiss()
                }
                Spacer()
                Button("Save") {
                    vm.updateEntry(entry)
                    dismiss()
                }
                .keyboardShortcut(.defaultAction)
            }
        }
        .padding()
        .frame(minWidth: 500, minHeight: 500)
    }
}
