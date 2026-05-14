import Foundation

struct AppRunHistory: Codable, Sendable, Identifiable {
    var id: UUID
    var appId: String
    var status: String
    var durationMs: Int
    var d3dBackend: String?
    var timestamp: Date
    var launcherJsonPath: String?

    static func make(result: LauncherResult, appId: String) -> AppRunHistory {
        AppRunHistory(
            id: UUID(),
            appId: appId,
            status: result.status ?? "UNKNOWN",
            durationMs: result.durationMs ?? 0,
            d3dBackend: result.d3dBackend,
            timestamp: Date(),
            launcherJsonPath: nil
        )
    }
}
