import CryptoKit
import Foundation

struct ProgramOverride: Codable, Equatable {
    var peHash: String
    var executableName: String
    var baseProfile: String
    var overrides: [String: String]

    enum CodingKeys: String, CodingKey {
        case peHash = "pe_hash"
        case executableName = "executable_name"
        case baseProfile = "base_profile"
        case overrides
    }
}

struct ProgramOverrideService {
    func root(bottleID: String, settings: AppSettings = .default) -> URL {
        URL(fileURLWithPath: settings.bottlesDirectory).appendingPathComponent(bottleID, isDirectory: true).appendingPathComponent("program", isDirectory: true)
    }

    func list(bottleID: String, settings: AppSettings = .default) throws -> [ProgramOverride] {
        let directory = root(bottleID: bottleID, settings: settings)
        if !FileManager.default.fileExists(atPath: directory.path) {
            try createFixture(bottleID: bottleID, settings: settings)
        }
        let files = (try? FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)) ?? []
        return files.filter { $0.lastPathComponent.hasSuffix(".override.json") }.compactMap { url in
            guard let data = try? Data(contentsOf: url) else { return nil }
            return try? JSONDecoder().decode(ProgramOverride.self, from: data)
        }.sorted { $0.executableName < $1.executableName }
    }

    func write(_ override: ProgramOverride, bottleID: String, settings: AppSettings = .default) throws {
        let directory = root(bottleID: bottleID, settings: settings)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        try JSONEncoder.pretty.encode(override).write(to: directory.appendingPathComponent("\(override.peHash).override.json"))
    }

    private func createFixture(bottleID: String, settings: AppSettings) throws {
        let hash = SHA256.hash(data: Data("\(bottleID)-fixture".utf8)).map { String(format: "%02x", $0) }.joined()
        try write(ProgramOverride(peHash: hash, executableName: "fixture.exe", baseProfile: "game-generic-dx11", overrides: ["graphics_backend": "dxvk", "WINEDEBUG": "-all"]), bottleID: bottleID, settings: settings)
    }
}
