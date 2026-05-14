import Foundation

@MainActor
final class CompatibilityStore {
    static let shared = CompatibilityStore()

    private let baseURL: URL = {
        let url = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
            .appendingPathComponent("MacRunnerControlCenter", isDirectory: true)
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }()

    var dbURL: URL { baseURL.appendingPathComponent("compatibility.json") }
    var historyURL: URL { baseURL.appendingPathComponent("run_history.json") }

    func loadEntries() -> [CompatibilityEntry] {
        guard let data = try? Data(contentsOf: dbURL) else { return [] }
        do {
            return try JSONDecoder().decode([CompatibilityEntry].self, from: data)
        } catch {
            return []
        }
    }

    func saveEntries(_ entries: [CompatibilityEntry]) {
        do {
            let data = try JSONEncoder().encode(entries)
            try data.write(to: dbURL)
        } catch {
            print("Failed to save compatibility DB: \(error)")
        }
    }

    func loadHistory() -> [AppRunHistory] {
        guard let data = try? Data(contentsOf: historyURL) else { return [] }
        do {
            return try JSONDecoder().decode([AppRunHistory].self, from: data)
        } catch {
            return []
        }
    }

    func saveHistory(_ history: [AppRunHistory]) {
        do {
            let data = try JSONEncoder().encode(history)
            try data.write(to: historyURL)
        } catch {
            print("Failed to save history: \(error)")
        }
    }

    func update(from app: AppEntry, result: LauncherResult?) {
        var entries = loadEntries()
        let id = app.id.uuidString
        if let idx = entries.firstIndex(where: { $0.id == id }) {
            var e = entries[idx]
            e.lastStatus = result?.status ?? app.lastRunStatus ?? e.lastStatus
            e.bestD3DBackend = app.d3dBackend
            e.lastRunDate = Date()
            if result?.status != "PASS" {
                e.failuresCount += 1
            }
            entries[idx] = e
        } else if let result = result {
            entries.append(CompatibilityEntry.make(app: app, result: result))
        }
        saveEntries(entries)

        if let result = result {
            var history = loadHistory()
            history.append(AppRunHistory.make(result: result, appId: id))
            saveHistory(history)
        }
    }

    func historyFor(appId: String) -> [AppRunHistory] {
        loadHistory().filter { $0.appId == appId }.sorted { $0.timestamp > $1.timestamp }
    }

    func passRate(appId: String) -> Double {
        let h = historyFor(appId: appId)
        guard !h.isEmpty else { return 0 }
        let passes = h.filter { $0.status == "PASS" }.count
        return Double(passes) / Double(h.count)
    }

    func isRegression(appId: String) -> Bool {
        let h = historyFor(appId: appId)
        guard h.count >= 2 else { return false }
        let latest = h[0]
        let previous = h[1]
        return latest.status != "PASS" && previous.status == "PASS"
    }

    func bestBackend(appId: String) -> String? {
        let h = historyFor(appId: appId)
        var passCounts: [String: Int] = [:]
        var totalCounts: [String: Int] = [:]
        for run in h {
            let backend = run.d3dBackend ?? "none"
            totalCounts[backend, default: 0] += 1
            if run.status == "PASS" {
                passCounts[backend, default: 0] += 1
            }
        }
        var best: (backend: String, rate: Double)?
        for (backend, total) in totalCounts where total >= 2 {
            let passes = passCounts[backend] ?? 0
            let rate = Double(passes) / Double(total)
            if best == nil || rate > best!.rate {
                best = (backend, rate)
            }
        }
        return best?.backend
    }
}
