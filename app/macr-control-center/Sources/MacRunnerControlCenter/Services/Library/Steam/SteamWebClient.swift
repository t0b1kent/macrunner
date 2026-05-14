import AppKit
import Foundation

struct SteamOwnedGame: Codable, Equatable {
    var appid: Int
    var name: String?
    var playtimeForever: Int?
    var imgIconURL: String?
    var lastPlayed: Date?

    enum CodingKeys: String, CodingKey {
        case appid
        case name
        case playtimeForever = "playtime_forever"
        case imgIconURL = "img_icon_url"
        case rtimeLastPlayed = "rtime_last_played"
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        appid = try c.decode(Int.self, forKey: .appid)
        name = try c.decodeIfPresent(String.self, forKey: .name)
        playtimeForever = try c.decodeIfPresent(Int.self, forKey: .playtimeForever)
        imgIconURL = try c.decodeIfPresent(String.self, forKey: .imgIconURL)
        if let seconds = try c.decodeIfPresent(TimeInterval.self, forKey: .rtimeLastPlayed), seconds > 0 {
            lastPlayed = Date(timeIntervalSince1970: seconds)
        } else {
            lastPlayed = nil
        }
    }

    init(appid: Int, name: String?, playtimeForever: Int?, imgIconURL: String?, lastPlayed: Date?) {
        self.appid = appid
        self.name = name
        self.playtimeForever = playtimeForever
        self.imgIconURL = imgIconURL
        self.lastPlayed = lastPlayed
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(appid, forKey: .appid)
        try c.encodeIfPresent(name, forKey: .name)
        try c.encodeIfPresent(playtimeForever, forKey: .playtimeForever)
        try c.encodeIfPresent(imgIconURL, forKey: .imgIconURL)
        if let lastPlayed { try c.encode(Int(lastPlayed.timeIntervalSince1970), forKey: .rtimeLastPlayed) }
    }
}

struct SteamRecentlyPlayedGame: Codable, Equatable {
    var appid: Int
    var name: String?
    var playtimeTwoWeeks: Int?
    var playtimeForever: Int?

    enum CodingKeys: String, CodingKey {
        case appid, name
        case playtimeTwoWeeks = "playtime_2weeks"
        case playtimeForever = "playtime_forever"
    }
}

struct SteamWebStatus: Codable, Equatable {
    var keySet: Bool
    var lastSync: String?
    var ownedGames: Int?
    var recentlyPlayed: Int?

    enum CodingKeys: String, CodingKey {
        case keySet = "key_set"
        case lastSync = "last_sync"
        case ownedGames = "owned_games"
        case recentlyPlayed = "recently_played"
    }
}

actor SteamWebRateLimiter {
    static let shared = SteamWebRateLimiter(minimumSpacing: 2.05)
    private let minimumSpacing: TimeInterval
    private var lastRequest: Date?

    init(minimumSpacing: TimeInterval) {
        self.minimumSpacing = minimumSpacing
    }

    func waitTurn() async {
        if let lastRequest {
            let delay = minimumSpacing - Date().timeIntervalSince(lastRequest)
            if delay > 0 {
                try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
            }
        }
        lastRequest = Date()
    }
}

struct SteamWebClient {
    var apiKey: String
    var steamID64: String
    var session: URLSession = .shared
    var baseURL: URL = URL(string: "https://api.steampowered.com")!

    func ownedGames() async throws -> [SteamOwnedGame] {
        let url = baseURL.appendingPathComponent("IPlayerService/GetOwnedGames/v1/")
            .appending(queryItems: [
                URLQueryItem(name: "key", value: apiKey),
                URLQueryItem(name: "steamid", value: steamID64),
                URLQueryItem(name: "include_appinfo", value: "true"),
                URLQueryItem(name: "include_played_free_games", value: "true"),
                URLQueryItem(name: "format", value: "json")
            ])
        let responseObject = try await getResponseObject(url: url)
        let gamesData = try JSONSerialization.data(withJSONObject: responseObject["games"] as? [[String: Any]] ?? [])
        return try JSONDecoder().decode([SteamOwnedGame].self, from: gamesData)
    }

    func recentlyPlayedGames() async throws -> [SteamRecentlyPlayedGame] {
        let url = baseURL.appendingPathComponent("IPlayerService/GetRecentlyPlayedGames/v1/")
            .appending(queryItems: [
                URLQueryItem(name: "key", value: apiKey),
                URLQueryItem(name: "steamid", value: steamID64),
                URLQueryItem(name: "count", value: "25"),
                URLQueryItem(name: "format", value: "json")
            ])
        let responseObject = try await getResponseObject(url: url)
        let gamesData = try JSONSerialization.data(withJSONObject: responseObject["games"] as? [[String: Any]] ?? [])
        return try JSONDecoder().decode([SteamRecentlyPlayedGame].self, from: gamesData)
    }

    func syncStatus(store: SteamWebStatusStore = SteamWebStatusStore()) async throws -> SteamWebStatus {
        let owned = try await ownedGames()
        let recent = (try? await recentlyPlayedGames()) ?? []
        let now = ISO8601DateFormatter().string(from: Date())
        let status = SteamWebStatus(keySet: true, lastSync: now, ownedGames: owned.count, recentlyPlayed: recent.count)
        try store.save(status)
        try store.saveRecent(recent)
        return status
    }

    func launchURL(appID: String) -> URL { URL(string: "steam://run/\(appID)")! }
    func installURL(appID: String) -> URL { URL(string: "steam://install/\(appID)")! }

    func coverURLs(appID: String) -> [URL] {
        SteamCoverFetcher().coverSources(appid: appID)
    }

    @MainActor
    func openSteam(appID: String, installed: Bool) {
        NSWorkspace.shared.open(installed ? launchURL(appID: appID) : installURL(appID: appID))
    }

    private func getResponseObject(url: URL) async throws -> [String: Any] {
        await SteamWebRateLimiter.shared.waitTurn()
        var request = URLRequest(url: url)
        request.setValue("MacRunner/1.0", forHTTPHeaderField: "User-Agent")
        let (data, response) = try await session.data(for: request)
        guard let http = response as? HTTPURLResponse else { throw SteamWebError.invalidResponse }
        guard (200..<300).contains(http.statusCode) else { throw SteamWebError.badStatus(http.statusCode) }
        let object = try JSONSerialization.jsonObject(with: data) as? [String: Any]
        return object?["response"] as? [String: Any] ?? [:]
    }
}

struct SteamWebStatusStore {
    var url: URL = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
        .appendingPathComponent("MacRunnerControlCenter/steam-web-status.json")

    func save(_ status: SteamWebStatus) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try JSONEncoder.pretty.encode(status).write(to: url)
    }

    func load() -> SteamWebStatus? {
        guard let data = try? Data(contentsOf: url) else { return nil }
        return try? JSONDecoder().decode(SteamWebStatus.self, from: data)
    }

    func saveRecent(_ games: [SteamRecentlyPlayedGame]) throws {
        let recentURL = url.deletingLastPathComponent().appendingPathComponent("steam-recently-played.json")
        try FileManager.default.createDirectory(at: recentURL.deletingLastPathComponent(), withIntermediateDirectories: true)
        try JSONEncoder.pretty.encode(games).write(to: recentURL)
    }

    func loadRecent() -> [SteamRecentlyPlayedGame] {
        let recentURL = url.deletingLastPathComponent().appendingPathComponent("steam-recently-played.json")
        guard let data = try? Data(contentsOf: recentURL) else { return [] }
        return (try? JSONDecoder().decode([SteamRecentlyPlayedGame].self, from: data)) ?? []
    }
}

enum SteamWebError: LocalizedError, Equatable {
    case invalidResponse
    case badStatus(Int)
    case missingSteamID64

    var errorDescription: String? {
        switch self {
        case .invalidResponse: return "Steam Web API response was not HTTP."
        case .badStatus(let status): return "Steam Web API returned HTTP \(status)."
        case .missingSteamID64: return "Steam Web API key is set, but SteamID64 is missing."
        }
    }
}
