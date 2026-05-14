import Foundation

struct SteamLibraryGame: Codable, Equatable {
    var appid: String
    var name: String
    var installed: Bool
    var installDir: String?
    var lastPlayed: String?
    var coverPath: String?

    enum CodingKeys: String, CodingKey {
        case appid, name, installed
        case installDir = "install_dir"
        case lastPlayed = "last_played"
        case coverPath = "cover_path"
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
            let cover = try? coverCache.localCover(provider: "steam", id: game.appID, title: game.name)
            return SteamLibraryGame(
                appid: game.appID,
                name: game.name,
                installed: game.installed,
                installDir: game.installPath,
                lastPlayed: game.lastPlayed.map(Self.isoDate),
                coverPath: cover?.path
            )
        }
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
