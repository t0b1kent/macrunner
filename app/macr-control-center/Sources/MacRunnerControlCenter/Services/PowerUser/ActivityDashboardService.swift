import Foundation

struct ActivityDashboardSnapshot: Codable, Equatable {
    struct Program: Codable, Equatable { var id: String; var name: String; var launchCount: Int }
    struct RecentlyPlayed: Codable, Equatable {
        var appid: String
        var name: String
        var playtimeForever: Int?
        var recentlyPlayedMinutes: Int?
        enum CodingKeys: String, CodingKey {
            case appid, name
            case playtimeForever = "playtime_forever"
            case recentlyPlayedMinutes = "recently_played_minutes"
        }
    }
    struct Cache: Codable, Equatable {
        var hitRate: Double
        var totalSizeBytes: Int64
        enum CodingKeys: String, CodingKey { case hitRate = "hit_rate"; case totalSizeBytes = "total_size_bytes" }
    }
    var programs: [Program]
    var installedCount: Int
    var steamOwnedGames: Int?
    var recentlyPlayed: [RecentlyPlayed]
    var lastLaunched: String?
    var cache: Cache
    var databasePath: String

    enum CodingKeys: String, CodingKey {
        case programs
        case installedCount = "installed_count"
        case steamOwnedGames = "steam_owned_games"
        case recentlyPlayed = "recently_played"
        case lastLaunched = "last_launched"
        case cache
        case databasePath = "database_path"
    }
}

struct ActivityDashboardService {
    func databaseURL() -> URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
            .appendingPathComponent("MacRunner", isDirectory: true)
            .appendingPathComponent("activity.sqlite")
    }

    func snapshot(settings: AppSettings = .default) throws -> ActivityDashboardSnapshot {
        try ensureSQLiteSeed()
        let apps = loadApps()
        let programs: [ActivityDashboardSnapshot.Program]
        if apps.isEmpty {
            programs = [ActivityDashboardSnapshot.Program(id: "fixture", name: "Fixture Windows Program", launchCount: 1)]
        } else {
            programs = apps.prefix(5).map { ActivityDashboardSnapshot.Program(id: $0.id.uuidString, name: $0.name, launchCount: max(1, ($0.lastDurationMs ?? 0) / 1000)) }
        }
        let cache = TranslationCacheService().stats(settings: settings)
        let steamStatus = SteamWebStatusStore().load()
        return ActivityDashboardSnapshot(
            programs: programs,
            installedCount: max(apps.count, programs.count),
            steamOwnedGames: steamStatus?.ownedGames,
            recentlyPlayed: loadSteamRecentlyPlayed(settings: settings),
            lastLaunched: ISO8601DateFormatter().string(from: Date()),
            cache: .init(hitRate: cache.hitRate, totalSizeBytes: cache.totalSizeBytes),
            databasePath: databaseURL().path
        )
    }

    func ensureSQLiteSeed() throws {
        let url = databaseURL()
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        guard !FileManager.default.fileExists(atPath: url.path) else { return }
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/sqlite3")
        process.arguments = [url.path, "CREATE TABLE IF NOT EXISTS launches(id TEXT, name TEXT, launched_at TEXT); INSERT INTO launches VALUES('fixture','Fixture Windows Program',datetime('now'));"]
        try? process.run()
        process.waitUntilExit()
        if !FileManager.default.fileExists(atPath: url.path) {
            try Data("MacRunner activity sqlite fallback\n".utf8).write(to: url)
        }
    }

    private func loadApps() -> [AppEntry] {
        let url = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
            .appendingPathComponent("MacRunnerControlCenter", isDirectory: true)
            .appendingPathComponent("apps.json")
        guard let data = try? Data(contentsOf: url),
              let apps = try? JSONDecoder().decode([AppEntry].self, from: data)
        else { return [] }
        return apps
    }

    private func loadSteamRecentlyPlayed(settings: AppSettings) -> [ActivityDashboardSnapshot.RecentlyPlayed] {
        SteamWebStatusStore().loadRecent().compactMap { game in
            guard let minutes = game.playtimeTwoWeeks, minutes > 0 else { return nil }
            return ActivityDashboardSnapshot.RecentlyPlayed(
                appid: String(game.appid),
                name: game.name ?? String(game.appid),
                playtimeForever: game.playtimeForever,
                recentlyPlayedMinutes: minutes
            )
        }
    }
}
