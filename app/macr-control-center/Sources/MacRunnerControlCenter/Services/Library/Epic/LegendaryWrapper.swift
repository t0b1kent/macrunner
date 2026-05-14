import Foundation

struct StoreGame: Identifiable, Codable, Equatable {
    var id: String
    var title: String
    var source: GameLibrarySource
    var installPath: String?
    var artURL: URL?
    var installed: Bool
}

struct StoreCommandPlan: Codable, Equatable {
    var executable: String
    var arguments: [String]
    var environment: [String: String]
}

struct LegendaryWrapper {
    var executable: String

    init(root: String = AppSettings.defaultRoot) {
        self.executable = StoreToolLocator.bundledTool("legendary") ?? "/opt/homebrew/bin/legendary"
    }

    func loginPlan() -> StoreCommandPlan {
        StoreCommandPlan(executable: executable, arguments: executable == "/usr/bin/env" ? ["legendary", "auth"] : ["auth"], environment: [:])
    }

    func listPlan() -> StoreCommandPlan {
        StoreCommandPlan(executable: executable, arguments: executable == "/usr/bin/env" ? ["legendary", "list", "--json"] : ["list", "--json"], environment: [:])
    }

    func launchPlan(appName: String) -> StoreCommandPlan {
        StoreCommandPlan(executable: executable, arguments: executable == "/usr/bin/env" ? ["legendary", "launch", appName] : ["launch", appName], environment: [:])
    }

    func parseListJSON(_ data: Data) -> [StoreGame] {
        guard let array = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]] else { return [] }
        return array.map { item in
            let id = item["app_name"] as? String ?? item["id"] as? String ?? UUID().uuidString
            return StoreGame(id: "epic:\(id)", title: item["app_title"] as? String ?? item["title"] as? String ?? id, source: .epic, installPath: item["install_path"] as? String, artURL: (item["image_url"] as? String).flatMap(URL.init(string:)), installed: item["is_installed"] as? Bool ?? false)
        }
    }
}
