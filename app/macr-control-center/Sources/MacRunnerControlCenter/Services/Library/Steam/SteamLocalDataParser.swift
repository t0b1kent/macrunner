import Foundation

struct SteamUser: Equatable {
    var steamID64: String
    var accountName: String
    var personaName: String
    var mostRecent: Bool
}

struct SteamLocalGame: Equatable {
    var appID: String
    var name: String
    var installPath: String?
    var lastPlayed: Date?
    var installed: Bool
}

struct SteamLocalDataParser {
    var steamRoot: URL = URL(fileURLWithPath: "~/Library/Application Support/Steam".macrExpandingTilde)
    private let parser = SteamVDFParser()

    func readUsers() -> [SteamUser] {
        let url = steamRoot.appendingPathComponent("config/loginusers.vdf")
        guard let text = try? String(contentsOf: url, encoding: .utf8),
              let root = try? parser.parse(text),
              let users = root["users"]?.objectValue
        else { return [] }
        return users.compactMap { steamID, node in
            guard let object = node.objectValue else { return nil }
            return SteamUser(
                steamID64: steamID,
                accountName: object["AccountName"]?.stringValue ?? "",
                personaName: object["PersonaName"]?.stringValue ?? "",
                mostRecent: object["MostRecent"]?.stringValue == "1"
            )
        }.sorted { $0.mostRecent && !$1.mostRecent }
    }

    func readInstalledGames() -> [SteamLocalGame] {
        let libraryRoots = readLibraryFolders()
        var games: [SteamLocalGame] = []
        for steamApps in libraryRoots {
            let manifests = ((try? FileManager.default.contentsOfDirectory(at: steamApps, includingPropertiesForKeys: nil)) ?? [])
                .filter { $0.lastPathComponent.hasPrefix("appmanifest_") && $0.pathExtension == "acf" }
            games.append(contentsOf: manifests.compactMap { parseManifest($0, steamApps: steamApps) })
        }
        return games.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }

    func readLibraryFolders() -> [URL] {
        let primary = steamRoot.appendingPathComponent("steamapps", isDirectory: true)
        let config = primary.appendingPathComponent("libraryfolders.vdf")
        guard let text = try? String(contentsOf: config, encoding: .utf8),
              let root = try? parser.parse(text),
              let folders = root["libraryfolders"]?.objectValue
        else { return FileManager.default.fileExists(atPath: primary.path) ? [primary] : [] }
        var urls = [primary]
        for (_, node) in folders {
            if let path = node.objectValue?["path"]?.stringValue {
                urls.append(URL(fileURLWithPath: path).appendingPathComponent("steamapps", isDirectory: true))
            }
        }
        return Array(Set(urls)).filter { FileManager.default.fileExists(atPath: $0.path) }
    }

    private func parseManifest(_ manifest: URL, steamApps: URL) -> SteamLocalGame? {
        guard let text = try? String(contentsOf: manifest, encoding: .utf8),
              let root = try? parser.parse(text),
              let state = root["AppState"]?.objectValue,
              let appID = state["appid"]?.stringValue
        else { return nil }
        let name = state["name"]?.stringValue ?? appID
        let installDir = state["installdir"]?.stringValue
        let installPath = installDir.map { steamApps.appendingPathComponent("common/\($0)").path }
        let lastPlayed = state["LastPlayed"]?.stringValue.flatMap { TimeInterval($0) }.map { Date(timeIntervalSince1970: $0) }
        return SteamLocalGame(appID: appID, name: name, installPath: installPath, lastPlayed: lastPlayed, installed: true)
    }
}

extension SteamLocalGame {
    func libraryItem() -> UnifiedLibraryItem {
        UnifiedLibraryItem(id: "steam:\(appID)", title: name, source: .steam, executablePath: nil, installPath: installPath, profileID: "game-steam-generic", bottleName: "steam-\(appID)", compatibilityBadge: "steam", lastPlayed: lastPlayed, isInstalled: installed)
    }
}
