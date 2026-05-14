import AppKit
import Foundation

struct SteamOwnedGame: Codable, Equatable {
    var appid: Int
    var name: String?
    var playtimeForever: Int?
    var imgIconURL: String?

    enum CodingKeys: String, CodingKey {
        case appid
        case name
        case playtimeForever = "playtime_forever"
        case imgIconURL = "img_icon_url"
    }
}

struct SteamWebClient {
    var apiKey: String
    var steamID64: String
    var session: URLSession = .shared
    var baseURL = URL(string: "https://api.steampowered.com")!

    func ownedGames() async throws -> [SteamOwnedGame] {
        let url = baseURL.appendingPathComponent("IPlayerService/GetOwnedGames/v1/")
            .appending(queryItems: [
                URLQueryItem(name: "key", value: apiKey),
                URLQueryItem(name: "steamid", value: steamID64),
                URLQueryItem(name: "include_appinfo", value: "true"),
                URLQueryItem(name: "format", value: "json")
            ])
        let (data, response) = try await session.data(from: url)
        guard let http = response as? HTTPURLResponse, (200..<300).contains(http.statusCode) else { throw SteamWebError.badStatus }
        let object = try JSONSerialization.jsonObject(with: data) as? [String: Any]
        let responseObject = object?["response"] as? [String: Any]
        let gamesData = try JSONSerialization.data(withJSONObject: responseObject?["games"] as? [[String: Any]] ?? [])
        return try JSONDecoder().decode([SteamOwnedGame].self, from: gamesData)
    }

    func launchURL(appID: String) -> URL { URL(string: "steam://run/\(appID)")! }
    func installURL(appID: String) -> URL { URL(string: "steam://install/\(appID)")! }

    func coverURLs(appID: String) -> [URL] {
        [
            URL(string: "https://cdn.cloudflare.steamstatic.com/steam/apps/\(appID)/library_600x900.jpg")!,
            URL(string: "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/\(appID)/library_hero.jpg")!
        ]
    }

    @MainActor
    func openSteam(appID: String, installed: Bool) {
        NSWorkspace.shared.open(installed ? launchURL(appID: appID) : installURL(appID: appID))
    }
}

enum SteamWebError: LocalizedError { case badStatus }
