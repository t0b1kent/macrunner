import SwiftUI

@MainActor
final class AppLibraryViewModel: ObservableObject {
    @Published var apps: [AppEntry] = []
    @Published var selectedApp: AppEntry?
    @Published var search = ""
    @Published var sortBy: SortOption = .name

    enum SortOption: String, CaseIterable {
        case name = "Name"
        case lastRun = "Last Run"
        case duration = "Duration"
        case status = "Status"
    }

    var filteredApps: [AppEntry] {
        var result = apps
        if !search.isEmpty {
            result = result.filter {
                $0.name.localizedCaseInsensitiveContains(search)
                    || $0.exePath.localizedCaseInsensitiveContains(search)
                    || ($0.tags ?? []).joined(separator: " ").localizedCaseInsensitiveContains(search)
            }
        }
        switch sortBy {
        case .name:
            result.sort { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
        case .lastRun:
            result.sort { ($0.updatedAt ?? .distantPast) > ($1.updatedAt ?? .distantPast) }
        case .duration:
            result.sort { ($0.lastDurationMs ?? 0) > ($1.lastDurationMs ?? 0) }
        case .status:
            result.sort { statusPriority($0.lastRunStatus) < statusPriority($1.lastRunStatus) }
        }
        return result
    }

    init() {
        apps = ConfigStore.shared.loadApps()
    }

    func add(_ app: AppEntry) {
        apps.append(app)
        save()
    }

    func update(_ app: AppEntry) {
        if let idx = apps.firstIndex(where: { $0.id == app.id }) {
            var updated = app
            updated.updatedAt = Date()
            apps[idx] = updated
            save()
        }
    }

    func delete(_ app: AppEntry) {
        apps.removeAll { $0.id == app.id }
        if selectedApp?.id == app.id { selectedApp = nil }
        save()
    }

    func quickRun(_ app: AppEntry, settings: AppSettings) {
        let vm = RunAppViewModel(settings: settings)
        vm.onComplete = { [weak self] result in
            var updated = app
            updated.lastRunStatus = result?.status
            updated.lastDurationMs = result?.durationMs
            updated.updatedAt = Date()
            self?.update(updated)
        }
        vm.run(app: app)
    }

    private func save() {
        ConfigStore.shared.saveApps(apps)
    }

    private func statusPriority(_ status: String?) -> Int {
        switch status {
        case "PASS": return 0
        case "FAIL": return 1
        case "CRASH": return 2
        case "TIMEOUT": return 3
        default: return 4
        }
    }
}
