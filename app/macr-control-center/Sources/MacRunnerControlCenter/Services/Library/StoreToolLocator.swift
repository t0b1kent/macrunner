import Foundation

struct StoreToolLocator {
    static func bundledTool(_ name: String) -> String? {
        let candidates = [
            Bundle.main.resourceURL?.appendingPathComponent("tools/\(name)").path,
            Bundle.module.resourceURL?.appendingPathComponent("tools/\(name)").path,
            URL(fileURLWithPath: AppSettings.defaultRoot).appendingPathComponent("app/macr-control-center/Sources/MacRunnerControlCenter/Resources/tools/\(name)").path
        ].compactMap { $0 }
        return candidates.first { FileManager.default.isExecutableFile(atPath: $0) }
    }

    static func toolPaths() -> [String: String] {
        [
            "legendary": bundledTool("legendary") ?? "/opt/homebrew/bin/legendary",
            "gogdl": bundledTool("gogdl") ?? "/opt/homebrew/bin/gogdl"
        ]
    }
}
