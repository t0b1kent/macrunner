import SwiftUI

struct AppLibraryView: View {
    @EnvironmentObject var settingsVM: SettingsViewModel
    @StateObject private var vm = AppLibraryViewModel()
    @State private var showAdd = false
    @State private var editingApp: AppEntry?

    var body: some View {
        NavigationSplitView {
            VStack(spacing: 0) {
                HStack {
                    Image(systemName: "magnifyingglass")
                        .foregroundStyle(.secondary)
                    TextField("Search apps", text: $vm.search)
                    if !vm.search.isEmpty {
                        Button {
                            vm.search = ""
                        } label: {
                            Image(systemName: "xmark.circle.fill")
                                .foregroundStyle(.secondary)
                        }
                        .buttonStyle(.plain)
                    }
                }
                .padding(8)
                .background(Color(.controlBackgroundColor))
                .cornerRadius(8)
                .padding(.horizontal)
                .padding(.top, 8)

                HStack {
                    Picker("Sort", selection: $vm.sortBy) {
                        ForEach(AppLibraryViewModel.SortOption.allCases, id: \.self) { option in
                            Text(option.rawValue).tag(option)
                        }
                    }
                    .pickerStyle(.segmented)
                    Spacer()
                    Text("\(vm.filteredApps.count) apps")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .padding(.horizontal)
                .padding(.vertical, 4)

                if vm.filteredApps.isEmpty {
                    ContentUnavailableView(
                        vm.search.isEmpty ? "No apps" : "No matches",
                        systemImage: vm.search.isEmpty ? "app.badge" : "magnifyingglass"
                    )
                    .frame(maxHeight: .infinity)
                } else {
                    List(selection: $vm.selectedApp) {
                        ForEach(vm.filteredApps) { app in
                            AppRowView(app: app)
                                .tag(app)
                                .contextMenu {
                                    Button("Run") {
                                        vm.quickRun(app, settings: settingsVM.settings)
                                    }
                                    Button("Edit") { editingApp = app }
                                    Button("Delete", role: .destructive) { vm.delete(app) }
                                }
                        }
                    }
                }
            }
            .navigationTitle("App Library")
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    Button("Run Selected", systemImage: "play.fill") {
                        if let app = vm.selectedApp {
                            vm.quickRun(app, settings: settingsVM.settings)
                        }
                    }
                    .disabled(vm.selectedApp == nil)
                }
                ToolbarItem(placement: .primaryAction) {
                    Button("Add App", systemImage: "plus") { showAdd = true }
                        .keyboardShortcut("n", modifiers: .command)
                }
            }
        } detail: {
            if let app = vm.selectedApp {
                AppDetailView(app: app, onUpdate: { vm.update($0) })
            } else {
                ContentUnavailableView("Select an app", systemImage: "app.badge")
            }
        }
        .sheet(isPresented: $showAdd) {
            AddAppView(onSave: { vm.add($0); showAdd = false })
        }
        .sheet(item: $editingApp) { app in
            AddAppView(app: app, onSave: { vm.update($0); editingApp = nil })
        }
    }
}

struct AppRowView: View {
    let app: AppEntry

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(app.name).font(.headline)
            Text(app.exePath).font(.caption).foregroundStyle(.secondary).lineLimit(1)
            HStack {
                Text(app.lastRunStatus ?? "Never run").font(.caption2)
                    .foregroundStyle(statusColor)
                if let ms = app.lastDurationMs {
                    Text("\(ms)ms").font(.caption2).foregroundStyle(.secondary)
                }
                Spacer()
                Text(app.d3dBackend).font(.caption2).foregroundStyle(.secondary)
                if let tags = app.tags, !tags.isEmpty {
                    Text(tags.joined(separator: ", "))
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
            }
        }
        .padding(.vertical, 2)
    }

    var statusColor: Color {
        switch app.lastRunStatus {
        case "PASS": return .green
        case "FAIL": return .red
        case "CRASH": return .orange
        case "TIMEOUT": return .yellow
        default: return .secondary
        }
    }
}
