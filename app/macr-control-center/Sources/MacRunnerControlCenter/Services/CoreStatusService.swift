import Foundation

struct CoreStatusService {
    static let requiredScripts = [
        "scripts/run-windows-app.sh",
        "scripts/run-integration-blocks.sh",
        "scripts/run-d3d-end-to-end-smoke.sh",
        "scripts/macr-doctor.sh",
        "scripts/verify-native-platform.sh",
        "scripts/run-real-app-corpus.sh",
        "scripts/assert-no-wine-leftovers.sh"
    ]

    static let optionalScripts = [
        "scripts/cleanup-wine-runtime.py",
        "scripts/list-wine-processes.sh",
        "scripts/run-performance-smoke.sh",
        "scripts/run-winapi-smoke-matrix.sh",
        "scripts/run-smoke-matrix.sh"
    ]

    static func check(root: String) -> CoreStatus {
        let fm = FileManager.default
        let rootURL = URL(fileURLWithPath: root)
        let isRootValid = fm.fileExists(atPath: root) && fm.fileExists(atPath: "\(root)/scripts/run-windows-app.sh")

        var missingScripts: [String] = []
        var scriptHealth: [String: Bool] = [:]

        for script in requiredScripts {
            let path = "\(root)/\(script)"
            let exists = fm.fileExists(atPath: path)
            scriptHealth[script] = exists
            if !exists {
                missingScripts.append(script)
            }
        }

        for script in optionalScripts {
            let path = "\(root)/\(script)"
            let exists = fm.fileExists(atPath: path)
            scriptHealth[script] = exists
        }

        let reportsDir = "\(root)/reports"
        let artifactsDir = "\(root)/artifacts"
        let missingReportsDir = !fm.fileExists(atPath: reportsDir)
        let missingArtifactsDir = !fm.fileExists(atPath: artifactsDir)

        if missingReportsDir {
            try? fm.createDirectory(atPath: reportsDir, withIntermediateDirectories: true)
        }
        if missingArtifactsDir {
            try? fm.createDirectory(atPath: artifactsDir, withIntermediateDirectories: true)
        }

        return CoreStatus(
            mode: isRootValid ? .localCore : .mock,
            rootPath: root,
            isRootValid: isRootValid,
            missingScripts: missingScripts,
            missingReportsDir: missingReportsDir,
            missingArtifactsDir: missingArtifactsDir,
            scriptHealth: scriptHealth,
            reportsDirPath: reportsDir,
            artifactsDirPath: artifactsDir,
            lastCheckedAt: Date()
        )
    }
}
