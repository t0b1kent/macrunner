import SwiftUI

@MainActor
final class CompatibilityDBViewModel: ObservableObject {
    @Published var entries: [CompatibilityEntry] = []
    @Published var search = ""
    @Published var filterStatus: CompatibilityStatus?
    @Published var filterArch = ""
    @Published var filterD3D = ""
    @Published var filterCategory: AppCategory?
    @Published var selectedEntry: CompatibilityEntry?
    @Published var showDetail = false

    var filtered: [CompatibilityEntry] {
        entries.filter { e in
            if let s = filterStatus, e.lastStatus?.lowercased() != s.rawValue.lowercased() { return false }
            if !filterArch.isEmpty, e.arch?.lowercased() != filterArch.lowercased() { return false }
            if !filterD3D.isEmpty, e.bestD3DBackend?.lowercased() != filterD3D.lowercased() { return false }
            if let c = filterCategory, e.category != c { return false }
            if search.isEmpty { return true }
            return e.name.localizedCaseInsensitiveContains(search)
                || (e.notes ?? "").localizedCaseInsensitiveContains(search)
                || (e.tags ?? []).joined(separator: " ").localizedCaseInsensitiveContains(search)
        }
    }

    func refresh() {
        entries = CompatibilityStore.shared.loadEntries()
    }

    func updateEntry(_ entry: CompatibilityEntry) {
        var current = CompatibilityStore.shared.loadEntries()
        if let idx = current.firstIndex(where: { $0.id == entry.id }) {
            current[idx] = entry
        } else {
            current.append(entry)
        }
        CompatibilityStore.shared.saveEntries(current)
        refresh()
    }

    func deleteEntry(_ entry: CompatibilityEntry) {
        var current = CompatibilityStore.shared.loadEntries()
        current.removeAll { $0.id == entry.id }
        CompatibilityStore.shared.saveEntries(current)
        refresh()
    }

    func exportJSON() {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.json]
        panel.nameFieldStringValue = "compatibility.json"
        if panel.runModal() == .OK, let url = panel.url {
            do {
                let data = try JSONEncoder().encode(filtered)
                try data.write(to: url)
            } catch { print("Export failed: \(error)") }
        }
    }

    func generateRegressionManifest(macRunnerRoot: String) -> String? {
        RegressionManifestService.saveRegressionsManifest(macRunnerRoot: macRunnerRoot)
    }

    func importJSON() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.json]
        if panel.runModal() == .OK, let url = panel.url,
           let data = try? Data(contentsOf: url),
           let imported = try? JSONDecoder().decode([CompatibilityEntry].self, from: data) {
            var current = CompatibilityStore.shared.loadEntries()
            for i in imported {
                if let idx = current.firstIndex(where: { $0.id == i.id }) {
                    current[idx] = i
                } else {
                    current.append(i)
                }
            }
            CompatibilityStore.shared.saveEntries(current)
            refresh()
        }
    }

    func passRate(for entry: CompatibilityEntry) -> Double {
        CompatibilityStore.shared.passRate(appId: entry.id)
    }

    func isRegression(_ entry: CompatibilityEntry) -> Bool {
        CompatibilityStore.shared.isRegression(appId: entry.id)
    }

    func suggestedBackend(for entry: CompatibilityEntry) -> String? {
        CompatibilityStore.shared.bestBackend(appId: entry.id)
    }

    func statusColor(_ status: String?) -> Color {
        switch status?.lowercased() {
        case "pass", "runs": return .green
        case "fail", "fails", "crashes": return .red
        case "runs_with_issues", "needs_fix", "needsd3d", "needsmetal": return .orange
        case "timeout", "times_out": return .purple
        default: return .secondary
        }
    }
}
