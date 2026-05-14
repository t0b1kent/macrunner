import Foundation

struct AppEntry: Codable, Sendable, Identifiable, Hashable {
    var id: UUID
    var name: String
    var exePath: String
    var arch: String?
    var args: [String]?
    var env: [String: String]?
    var workdir: String?
    var d3dBackend: String
    var timeout: Int?
    var tags: [String]?
    var createdAt: Date
    var updatedAt: Date
    var lastRunStatus: String?
    var lastDurationMs: Int?
    var notes: String?

    static func new(name: String, exePath: String) -> AppEntry {
        AppEntry(
            id: UUID(),
            name: name,
            exePath: exePath,
            arch: nil,
            args: [],
            env: [:],
            workdir: nil,
            d3dBackend: "none",
            timeout: 45,
            tags: [],
            createdAt: Date(),
            updatedAt: Date(),
            lastRunStatus: nil,
            lastDurationMs: nil,
            notes: nil
        )
    }
}
