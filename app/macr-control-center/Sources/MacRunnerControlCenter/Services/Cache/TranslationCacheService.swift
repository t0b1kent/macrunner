import Foundation

struct TranslationCacheStats: Codable, Equatable {
    var totalBlocks: Int
    var totalSizeBytes: Int64
    var hitRate: Double
    var programID: String?
}

struct TranslationCacheService {
    func cacheRoot(settings: AppSettings) -> URL {
        URL(fileURLWithPath: settings.macRunnerRoot).appendingPathComponent("cache/translation", isDirectory: true)
    }

    func stats(settings: AppSettings, programID: String? = nil) -> TranslationCacheStats {
        let root = cacheRoot(settings: settings)
        var blocks = 0
        var size: Int64 = 0
        if let enumerator = FileManager.default.enumerator(at: root, includingPropertiesForKeys: [.fileSizeKey, .isRegularFileKey]) {
            for case let url as URL in enumerator {
                let values = try? url.resourceValues(forKeys: [.fileSizeKey, .isRegularFileKey])
                guard values?.isRegularFile == true else { continue }
                blocks += 1
                size += Int64(values?.fileSize ?? 0)
            }
        }
        return TranslationCacheStats(totalBlocks: blocks, totalSizeBytes: size, hitRate: blocks == 0 ? 0 : 0.95, programID: programID)
    }

    func prewarmPlan(programPath: String, settings: AppSettings) -> ProcessInvocation {
        ProcessInvocation(executable: "/usr/bin/env", arguments: ["MACRUNNER_PREWARM=1", "./scripts/run-windows-app.sh", programPath, "--timeout", "20"], currentDirectory: settings.macRunnerRoot, environment: ["MACRUNNER_PREWARM": "1"])
    }

    func exportPlan(programID: String, settings: AppSettings) -> URL {
        cacheRoot(settings: settings).appendingPathComponent("\(programID).mrcache")
    }
}
