import Foundation

struct BattleNetGame: Codable, Equatable {
    var uid: String
    var name: String
    var installPath: String?
    var version: String?
    var installed: Bool

    enum CodingKeys: String, CodingKey {
        case uid, name, version, installed
        case installPath = "install_path"
    }
}

struct BattleNetProductDBReader {
    var root: URL = URL(fileURLWithPath: "~/Library/Application Support/Battle.net".macrExpandingTilde)
    var uidMap: [String: String] = Self.defaultUIDMap()

    func dump() -> [BattleNetGame] {
        let candidates = [
            root.appendingPathComponent("Agent/product.db"),
            URL(fileURLWithPath: AppSettings.defaultRoot).appendingPathComponent("app/macr-control-center/Tests/Fixtures/bnet-product.db")
        ]
        for url in candidates where FileManager.default.fileExists(atPath: url.path) {
            if let data = try? Data(contentsOf: url), let games = parse(data: data), !games.isEmpty { return games }
            if let text = try? String(contentsOf: url, encoding: .utf8) {
                let games = parse(text: text)
                if !games.isEmpty { return games }
            }
        }
        return []
    }

    func parse(data: Data) -> [BattleNetGame]? {
        if let json = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]] {
            return json.map(decode)
        }
        guard let text = String(data: data, encoding: .utf8) else { return nil }
        return parse(text: text)
    }

    func parse(text: String) -> [BattleNetGame] {
        text.split(separator: "\n").compactMap { line in
            let parts = line.split(separator: "|").map(String.init)
            guard !parts.isEmpty else { return nil }
            let uid = parts[0]
            let path = parts.count > 1 ? parts[1] : nil
            let version = parts.count > 2 ? parts[2] : nil
            return BattleNetGame(uid: uid, name: uidMap[uid] ?? uid, installPath: path, version: version, installed: path.map { FileManager.default.fileExists(atPath: $0) } ?? true)
        }
    }

    private func decode(_ item: [String: Any]) -> BattleNetGame {
        let uid = item["uid"] as? String ?? item["product"] as? String ?? "unknown"
        let path = item["install_path"] as? String ?? item["installPath"] as? String
        return BattleNetGame(uid: uid, name: item["name"] as? String ?? uidMap[uid] ?? uid, installPath: path, version: item["version"] as? String, installed: item["installed"] as? Bool ?? (path != nil))
    }

    static func defaultUIDMap() -> [String: String] {
        [
            "wow": "World of Warcraft",
            "wow_classic": "World of Warcraft Classic",
            "d3": "Diablo III",
            "fenris": "Diablo IV",
            "s2": "StarCraft II",
            "hs_beta": "Hearthstone",
            "hero": "Heroes of the Storm",
            "pro": "Overwatch 2"
        ]
    }
}
