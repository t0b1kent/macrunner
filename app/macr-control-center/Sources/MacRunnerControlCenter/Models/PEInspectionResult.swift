import Foundation

struct PEInspectionResult: Codable, Sendable {
    var arch: String
    var subsystem: String
    var d3dDetected: Bool
    var suggestedD3DBackend: String
    var suggestedTimeout: Int
    var imports: [String]?
    var exports: [String]?
    var sections: [String]?
    var dllDependencies: [String]?
    var isDotNet: Bool?
    var is64Bit: Bool?
    var fileSizeBytes: Int64?
    var iconPath: String?

    // Classification
    var category: AppCategory?
    var hasGraphics: Bool?
    var hasAudio: Bool?
    var hasInput: Bool?
    var hasNetwork: Bool?
    var hasCOM: Bool?
    var hasInstaller: Bool?
    var d3dVersion: String?
    var graphicsAPI: String?
    var audioAPI: String?
    var inputAPI: String?
    var suggestedLane: String?

    static let `default` = PEInspectionResult(
        arch: "unknown",
        subsystem: "unknown",
        d3dDetected: false,
        suggestedD3DBackend: "none",
        suggestedTimeout: 45,
        imports: nil,
        exports: nil,
        sections: nil,
        dllDependencies: nil,
        isDotNet: nil,
        is64Bit: nil,
        fileSizeBytes: nil,
        iconPath: nil
    )
}
