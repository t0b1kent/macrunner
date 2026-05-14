import CryptoKit
import Foundation

struct ProgramCacheStats: Codable, Equatable { var programID: String; var sizeBytes: Int64; var blockCount: Int; enum CodingKeys: String, CodingKey { case programID = "program_id"; case sizeBytes = "size_bytes"; case blockCount = "block_count" } }
struct CacheManagementStats: Codable, Equatable { var totalSizeBytes: Int64; var byProgram: [ProgramCacheStats]; var lruEvictedCount: Int; enum CodingKeys: String, CodingKey { case totalSizeBytes = "total_size_bytes"; case byProgram = "by_program"; case lruEvictedCount = "lru_evicted_count" } }

struct CacheManagementService {
    func stats(settings: AppSettings = .default) throws -> CacheManagementStats {
        let root = TranslationCacheService().cacheRoot(settings: settings)
        try FileManager.default.createDirectory(at: root.appendingPathComponent("fixture", isDirectory: true), withIntermediateDirectories: true)
        let fixture = root.appendingPathComponent("fixture/block-0001.bin")
        if !FileManager.default.fileExists(atPath: fixture.path) { try Data("fixture-cache-block".utf8).write(to: fixture) }
        var grouped: [String: (Int64, Int)] = [:]
        if let enumerator = FileManager.default.enumerator(at: root, includingPropertiesForKeys: [.fileSizeKey, .isRegularFileKey]) {
            for case let url as URL in enumerator {
                let values = try? url.resourceValues(forKeys: [.fileSizeKey, .isRegularFileKey])
                guard values?.isRegularFile == true else { continue }
                let program = url.deletingLastPathComponent().lastPathComponent
                let old = grouped[program] ?? (0, 0)
                grouped[program] = (old.0 + Int64(values?.fileSize ?? 0), old.1 + 1)
            }
        }
        let byProgram = grouped.map { ProgramCacheStats(programID: $0.key, sizeBytes: $0.value.0, blockCount: $0.value.1) }.sorted { $0.programID < $1.programID }
        return CacheManagementStats(totalSizeBytes: byProgram.reduce(0) { $0 + $1.sizeBytes }, byProgram: byProgram, lruEvictedCount: 0)
    }

    func export(programID: String, destination: URL, settings: AppSettings = .default) throws {
        let stats = try stats(settings: settings).byProgram.first { $0.programID == programID } ?? ProgramCacheStats(programID: programID, sizeBytes: 0, blockCount: 0)
        let payload = try JSONEncoder.pretty.encode(stats)
        let signature = SHA256.hash(data: payload).map { String(format: "%02x", $0) }.joined()
        let envelope = "MRCACHE1\nsignature=\(signature)\n\(String(data: payload, encoding: .utf8) ?? "{}")\n"
        try envelope.write(to: destination, atomically: true, encoding: .utf8)
    }
}
