import Foundation

struct BottleDeepSettings: Codable, Equatable {
    var bottleID: String
    var dllOverrides: [String: String]
    var envVars: [String: String]
    var registryTweaks: [String: String]
    var wineVersion: String
    var graphicsBackend: String
    var translationCache: String?

    enum CodingKeys: String, CodingKey {
        case bottleID = "bottle_id"
        case dllOverrides = "dll_overrides"
        case envVars = "env_vars"
        case registryTweaks = "registry_tweaks"
        case wineVersion = "wine_version"
        case graphicsBackend = "graphics_backend"
        case translationCache = "translation_cache"
    }
}

struct BottleDeepSettingsService {
    func settingsURL(bottleID: String, settings: AppSettings = .default) -> URL {
        URL(fileURLWithPath: settings.bottlesDirectory).appendingPathComponent(bottleID, isDirectory: true).appendingPathComponent("bottle.json")
    }

    func loadOrCreate(bottleID: String, settings: AppSettings = .default) throws -> BottleDeepSettings {
        let url = settingsURL(bottleID: bottleID, settings: settings)
        if let data = try? Data(contentsOf: url), let decoded = try? JSONDecoder().decode(BottleDeepSettings.self, from: data) {
            return decoded
        }
        let defaults = BottleDeepSettings(bottleID: bottleID, dllOverrides: ["d3d11": "native,builtin"], envVars: ["WINEDEBUG": "-all"], registryTweaks: ["HKCU\\Software\\Wine\\MacRunner": "enabled"], wineVersion: "bundled", graphicsBackend: settings.defaultD3DBackend == "none" ? "auto" : settings.defaultD3DBackend, translationCache: nil)
        try save(defaults, settings: settings)
        return defaults
    }

    func save(_ deep: BottleDeepSettings, settings: AppSettings = .default) throws {
        let url = settingsURL(bottleID: deep.bottleID, settings: settings)
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        let data = try JSONEncoder.pretty.encode(deep)
        try data.write(to: url)
    }
}
