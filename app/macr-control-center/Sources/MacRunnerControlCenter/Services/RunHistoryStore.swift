import Foundation

@MainActor
final class RunHistoryStore {
    static let shared = RunHistoryStore()

    private let baseURL: URL = {
        let url = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
            .appendingPathComponent("MacRunnerControlCenter", isDirectory: true)
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }()

    private var historyURL: URL { baseURL.appendingPathComponent("history.json") }
    private let maxEntries = 200

    func load() -> [MacRunnerTask] {
        guard let data = try? Data(contentsOf: historyURL) else { return [] }
        do {
            return try JSONDecoder().decode([MacRunnerTask].self, from: data)
        } catch {
            print("Failed to load history: \(error)")
            return []
        }
    }

    func append(_ task: MacRunnerTask) {
        var entries = load()
        entries.insert(task, at: 0)
        if entries.count > maxEntries {
            entries = Array(entries.prefix(maxEntries))
        }
        save(entries)
    }

    func remove(taskId: UUID) {
        var entries = load()
        entries.removeAll { $0.id == taskId }
        save(entries)
    }

    func clear() {
        save([])
    }

    private func save(_ entries: [MacRunnerTask]) {
        do {
            let data = try JSONEncoder().encode(entries)
            try data.write(to: historyURL)
        } catch {
            print("Failed to save history: \(error)")
        }
    }
}
