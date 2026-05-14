import Foundation

@MainActor
final class ConfigStore {
    static let shared = ConfigStore()

    private let baseURL: URL = {
        let url = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
            .appendingPathComponent("MacRunnerControlCenter", isDirectory: true)
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }()

    var appsURL: URL { baseURL.appendingPathComponent("apps.json") }
    var settingsURL: URL { baseURL.appendingPathComponent("settings.json") }

    func loadApps() -> [AppEntry] {
        guard let data = try? Data(contentsOf: appsURL) else { return [] }
        do {
            return try JSONDecoder().decode([AppEntry].self, from: data)
        } catch {
            return []
        }
    }

    func saveApps(_ apps: [AppEntry]) {
        do {
            let data = try JSONEncoder().encode(apps)
            try data.write(to: appsURL)
        } catch {
            print("Failed to save apps: \(error)")
        }
    }

    func loadSettings() -> AppSettings {
        guard let data = try? Data(contentsOf: settingsURL) else { return .default }
        do {
            return try JSONDecoder().decode(AppSettings.self, from: data)
        } catch {
            return .default
        }
    }

    func saveSettings(_ settings: AppSettings) {
        do {
            let data = try JSONEncoder().encode(settings)
            try data.write(to: settingsURL)
        } catch {
            print("Failed to save settings: \(error)")
        }
    }
}
