import Foundation

enum CompatibilityStatus: String, Codable, Sendable, CaseIterable {
    case unknown
    case runs
    case runsWithIssues
    case fails
    case crashes
    case timesOut
    case needsD3D
    case needsMetal
    case needsFix
}

struct CompatibilityEntry: Codable, Sendable, Identifiable, Hashable {
    var id: String
    var name: String
    var exePathHash: String
    var arch: String?
    var lastStatus: String?
    var bestD3DBackend: String?
    var lastSuccessfulVersion: String?
    var failuresCount: Int
    var notes: String?
    var tags: [String]?
    var lastRunDate: Date?
    var category: AppCategory?
    var artifactPaths: [String]?

    static func make(app: AppEntry, result: LauncherResult?) -> CompatibilityEntry {
        CompatibilityEntry(
            id: app.id.uuidString,
            name: app.name,
            exePathHash: app.exePath,
            arch: result?.arch ?? result?.machine ?? app.arch ?? "unknown",
            lastStatus: result?.status ?? app.lastRunStatus ?? "unknown",
            bestD3DBackend: app.d3dBackend,
            lastSuccessfulVersion: nil,
            failuresCount: (result?.status != "PASS") ? 1 : 0,
            notes: app.notes,
            tags: app.tags,
            lastRunDate: Date(),
            category: nil,
            artifactPaths: []
        )
    }
}

enum AppCategory: String, Codable, Sendable {
    case unknown
    case console
    case gui
    case gdi
    case d3d11
    case d3d12
    case installer
    case gameDemo = "game/demo"
}
