import Foundation

struct SteamLibraryGame: Codable, Equatable {
    var appid: String
    var name: String
    var installed: Bool
    var installDir: String?
    var lastPlayed: String?
    var coverPath: String?
    var installState: String?
    var playtimeForever: Int?
    var recentlyPlayedMinutes: Int?

    enum CodingKeys: String, CodingKey {
        case appid, name, installed
        case installDir = "install_dir"
        case lastPlayed = "last_played"
        case coverPath = "cover_path"
        case installState = "install_state"
        case playtimeForever = "playtime_forever"
        case recentlyPlayedMinutes = "recently_played_minutes"
    }
}

struct SteamLibraryService {
    var parser: SteamLocalDataParser
    var coverCache = CoverCache()

    init(root: URL? = nil) {
        self.parser = SteamLocalDataParser(steamRoot: root ?? URL(fileURLWithPath: "~/Library/Application Support/Steam".macrExpandingTilde))
    }

    func dump() -> [SteamLibraryGame] {
        let games = parser.readInstalledGames()
        return games.map { game in
            let cover = coverCache.cachedCover(provider: "steam", id: game.appID)
            return SteamLibraryGame(
                appid: game.appID,
                name: game.name,
                installed: game.installed,
                installDir: game.installPath,
                lastPlayed: game.lastPlayed.map(Self.isoDate),
                coverPath: cover?.path,
                installState: game.installed ? "installed" : "not_installed",
                playtimeForever: nil,
                recentlyPlayedMinutes: nil
            )
        }
    }

    func dumpEnriched(settings: AppSettings = .default) async throws -> [SteamLibraryGame] {
        guard let key = SteamKeyStore().loadAPIKey(), !key.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            return dump()
        }
        guard let steamID = currentSteamID64(settings: settings) else {
            throw SteamWebError.missingSteamID64
        }
        let client = SteamWebClient(apiKey: key, steamID64: steamID)
        let owned = try await client.ownedGames()
        let recent = (try? await client.recentlyPlayedGames()) ?? []
        let installed = Dictionary(uniqueKeysWithValues: parser.readInstalledGames().map { ($0.appID, $0) })
        let recentByID = Dictionary(uniqueKeysWithValues: recent.map { (String($0.appid), $0.playtimeTwoWeeks ?? 0) })
        return owned.map { game in
            let appid = String(game.appid)
            let local = installed[appid]
            let cover = coverCache.cachedCover(provider: "steam", id: appid)
            return SteamLibraryGame(
                appid: appid,
                name: game.name ?? local?.name ?? appid,
                installed: local?.installed ?? false,
                installDir: local?.installPath,
                lastPlayed: local?.lastPlayed.map(Self.isoDate) ?? game.lastPlayed.map(Self.isoDate),
                coverPath: cover?.path,
                installState: local?.installed == true ? "installed" : "owned_not_installed",
                playtimeForever: game.playtimeForever,
                recentlyPlayedMinutes: recentByID[appid]
            )
        }.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }

    func currentSteamID64(settings: AppSettings = .default) -> String? {
        if let explicit = settings.steamID64?.trimmingCharacters(in: .whitespacesAndNewlines), !explicit.isEmpty {
            return explicit
        }
        return parser.readUsers().first?.steamID64
    }

    static func fixtureRoot(settings: AppSettings = .default) -> URL {
        URL(fileURLWithPath: settings.macRunnerRoot).appendingPathComponent("app/macr-control-center/Tests/Fixtures/steam-account", isDirectory: true)
    }

    static func ensureFixtureIfNeeded(settings: AppSettings = .default) throws -> URL {
        let real = URL(fileURLWithPath: "~/Library/Application Support/Steam".macrExpandingTilde)
        if FileManager.default.fileExists(atPath: real.appendingPathComponent("steamapps").path) { return real }
        let root = fixtureRoot(settings: settings)
        let steamapps = root.appendingPathComponent("steamapps", isDirectory: true)
        try FileManager.default.createDirectory(at: steamapps, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: root.appendingPathComponent("config", isDirectory: true), withIntermediateDirectories: true)
        try "\"users\" { \"76561198000000000\" { \"AccountName\" \"fixture\" \"PersonaName\" \"Fixture User\" \"MostRecent\" \"1\" } }\n".write(to: root.appendingPathComponent("config/loginusers.vdf"), atomically: true, encoding: .utf8)
        try "\"libraryfolders\" { \"0\" { \"path\" \"\(root.path.replacingOccurrences(of: "\\", with: "\\\\"))\" } }\n".write(to: steamapps.appendingPathComponent("libraryfolders.vdf"), atomically: true, encoding: .utf8)
        try "\"AppState\" { \"appid\" \"620\" \"name\" \"Portal 2 Fixture\" \"installdir\" \"Portal 2\" \"SizeOnDisk\" \"123456\" \"LastPlayed\" \"1710000000\" }\n".write(to: steamapps.appendingPathComponent("appmanifest_620.acf"), atomically: true, encoding: .utf8)
        return root
    }

    private static func isoDate(_ date: Date) -> String {
        ISO8601DateFormatter().string(from: date)
    }
}
