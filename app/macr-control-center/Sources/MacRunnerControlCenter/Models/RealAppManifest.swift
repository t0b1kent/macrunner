import Foundation

struct RealAppManifest: Codable, Sendable {
    var schemaVersion: Int?
    var apps: [ManifestApp]?
}

struct ManifestApp: Codable, Sendable, Identifiable, Hashable {
    var id = UUID()
    var name: String?
    var path: String?
    var arch: String?
    var args: [String]?
    var env: [String: String]?
    var workdir: String?
    var expectedRc: Int?
    var expectedStdoutContains: String?
    var expectedFiles: [String]?
    var d3dBackend: String?
    var allowGui: Bool?
    var timeout: Int?

    init(id: UUID = UUID(), name: String? = nil, path: String? = nil, arch: String? = nil, args: [String]? = nil, env: [String: String]? = nil, workdir: String? = nil, expectedRc: Int? = nil, expectedStdoutContains: String? = nil, expectedFiles: [String]? = nil, d3dBackend: String? = nil, allowGui: Bool? = nil, timeout: Int? = nil) {
        self.id = id
        self.name = name
        self.path = path
        self.arch = arch
        self.args = args
        self.env = env
        self.workdir = workdir
        self.expectedRc = expectedRc
        self.expectedStdoutContains = expectedStdoutContains
        self.expectedFiles = expectedFiles
        self.d3dBackend = d3dBackend
        self.allowGui = allowGui
        self.timeout = timeout
    }

    enum CodingKeys: String, CodingKey {
        case name
        case path
        case arch
        case args
        case env
        case workdir
        case expectedRc = "expected_rc"
        case expectedStdoutContains = "expected_stdout_contains"
        case expectedFiles = "expected_files"
        case d3dBackend = "d3d_backend"
        case allowGui = "allow_gui"
        case timeout
    }
}
