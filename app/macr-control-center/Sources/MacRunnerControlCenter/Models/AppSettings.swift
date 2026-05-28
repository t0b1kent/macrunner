struct AppSettings: Codable, Sendable {
    var macRunnerRoot: String
    var defaultTimeout: Int
    var defaultD3DBackend: String
    var keepArtifactsDefault: Bool
    var doctorTimeout: Int
    var integrationTimeout: Int
    var artifactsDirectory: String
    var bottlesDirectory: String
    var enableDebugLogs: Bool
    var enableMockMode: Bool
    var anthropicModelOverride: String? = nil
    var steamID64: String? = nil
    var languageOverride: String? = nil
    var updateChannel: String? = nil
    var hudHotkey: String? = nil
    var telemetryOptIn: Bool? = nil
    var communityFixSharingOptIn: Bool? = nil

    static let defaultRoot = "/Users/timurtoby/Documents/MacRunner/Main/MacRunner"

    static let `default` = AppSettings(
        macRunnerRoot: defaultRoot,
        defaultTimeout: 45,
        defaultD3DBackend: "none",
        keepArtifactsDefault: false,
        doctorTimeout: 120,
        integrationTimeout: 300,
        artifactsDirectory: "\(defaultRoot)/artifacts",
        bottlesDirectory: "\(defaultRoot)/bottles",
        enableDebugLogs: false,
        enableMockMode: false
    )
}
