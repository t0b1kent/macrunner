import Foundation

enum CoreMode: String, Codable, Sendable, CaseIterable {
    case mock = "Mock"
    case localCore = "Local Core"
    case detachedReports = "Detached Reports"
}

struct CoreStatus: Codable, Sendable {
    var mode: CoreMode
    var rootPath: String
    var isRootValid: Bool
    var missingScripts: [String]
    var missingReportsDir: Bool
    var missingArtifactsDir: Bool
    var scriptHealth: [String: Bool]
    var reportsDirPath: String
    var artifactsDirPath: String
    var lastCheckedAt: Date?

    var isConnected: Bool {
        isRootValid && missingScripts.isEmpty && !missingReportsDir && !missingArtifactsDir
    }

    var healthScore: Int {
        var score = 0
        let max = scriptHealth.count + 3
        if isRootValid { score += 1 }
        if missingScripts.isEmpty { score += 1 }
        if !missingReportsDir { score += 1 }
        if !missingArtifactsDir { score += 1 }
        for (_, ok) in scriptHealth {
            if ok { score += 1 }
        }
        return max > 0 ? Int((Double(score) / Double(max)) * 100) : 0
    }

    var statusDescription: String {
        if isConnected { return "Connected" }
        if !isRootValid { return "Invalid Root" }
        if !missingScripts.isEmpty { return "\(missingScripts.count) Missing Scripts" }
        if missingReportsDir || missingArtifactsDir { return "Missing Directories" }
        return "Unknown"
    }

    static func unknown(root: String) -> CoreStatus {
        CoreStatus(
            mode: .mock,
            rootPath: root,
            isRootValid: false,
            missingScripts: [],
            missingReportsDir: true,
            missingArtifactsDir: true,
            scriptHealth: [:],
            reportsDirPath: "",
            artifactsDirPath: "",
            lastCheckedAt: nil
        )
    }
}
