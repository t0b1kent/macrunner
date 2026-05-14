import Foundation

struct GogdlWrapper {
    var executable: String

    init(root: String = AppSettings.defaultRoot) {
        self.executable = StoreToolLocator.bundledTool("gogdl") ?? "/opt/homebrew/bin/gogdl"
    }

    func loginPlan() -> StoreCommandPlan {
        StoreCommandPlan(executable: executable, arguments: executable == "/usr/bin/env" ? ["gogdl", "auth"] : ["auth"], environment: [:])
    }

    func listPlan() -> StoreCommandPlan {
        StoreCommandPlan(executable: executable, arguments: executable == "/usr/bin/env" ? ["gogdl", "list", "--json"] : ["list", "--json"], environment: [:])
    }

    func launchPlan(productID: String, executablePath: String?) -> StoreCommandPlan {
        if let executablePath {
            return StoreCommandPlan(executable: "/usr/bin/open", arguments: [executablePath], environment: [:])
        }
        return StoreCommandPlan(executable: executable, arguments: executable == "/usr/bin/env" ? ["gogdl", "launch", productID] : ["launch", productID], environment: [:])
    }

    func parseListJSON(_ data: Data) -> [StoreGame] {
        guard let array = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]] else { return [] }
        return array.map { item in
            let id = String(describing: item["id"] ?? item["product_id"] ?? UUID().uuidString)
            return StoreGame(id: "gog:\(id)", title: item["title"] as? String ?? item["name"] as? String ?? id, source: .gog, installPath: item["install_path"] as? String, artURL: (item["cover"] as? String).flatMap(URL.init(string:)), installed: item["installed"] as? Bool ?? false)
        }
    }
}
