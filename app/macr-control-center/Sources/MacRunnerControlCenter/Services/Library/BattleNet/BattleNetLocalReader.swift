import Foundation

struct BattleNetLocalReader {
    var battleNetRoot: URL = URL(fileURLWithPath: "~/Library/Application Support/Battle.net".macrExpandingTilde)

    func readInstalledGames() -> [StoreGame] {
        var games: [StoreGame] = []
        let config = battleNetRoot.appendingPathComponent("Battle.net.config")
        if let data = try? Data(contentsOf: config),
           let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
            games.append(contentsOf: parseConfig(object))
        }
        let productDB = battleNetRoot.appendingPathComponent("Agent/product.db")
        if let text = try? String(contentsOf: productDB, encoding: .utf8) {
            games.append(contentsOf: parseProductDB(text))
        }
        var unique: [String: StoreGame] = [:]
        for game in games { unique[game.id] = game }
        return unique.values.sorted { $0.title.localizedCaseInsensitiveCompare($1.title) == .orderedAscending }
    }

    private func parseConfig(_ object: [String: Any]) -> [StoreGame] {
        let products = object["Games"] as? [String: Any] ?? object["games"] as? [String: Any] ?? [:]
        return products.compactMap { key, value in
            let dict = value as? [String: Any] ?? [:]
            let path = dict["InstallPath"] as? String ?? dict["install_path"] as? String
            return StoreGame(id: "battlenet:\(key)", title: dict["Name"] as? String ?? key, source: .battleNet, installPath: path, artURL: nil, installed: path.map { FileManager.default.fileExists(atPath: $0) } ?? true)
        }
    }

    private func parseProductDB(_ text: String) -> [StoreGame] {
        text.split(separator: "\n").compactMap { line in
            guard line.contains("install") || line.contains("product") else { return nil }
            let title = line.split(separator: "|").first.map(String.init) ?? String(line.prefix(32))
            return StoreGame(id: "battlenet:\(title.lowercased().replacingOccurrences(of: " ", with: "-"))", title: title, source: .battleNet, installPath: nil, artURL: nil, installed: true)
        }
    }
}
