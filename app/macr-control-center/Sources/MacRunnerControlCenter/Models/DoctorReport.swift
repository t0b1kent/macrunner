struct DoctorReport: Codable, Sendable {
    var schemaVersion: Int?
    var host: DoctorHost?
    var tools: DoctorTools?
    var lanes: DoctorLanes?
    var graphics: DoctorGraphics?
    var scripts: DoctorScripts?
}

struct DoctorHost: Codable, Sendable {
    var system: String?
    var machine: String?
    var macos: String?
}

struct DoctorTools: Codable, Sendable {
    var clang: Bool?
    var python: String?
}

struct DoctorLanes: Codable, Sendable {
    var arm64: LaneStatus?
    var x64: LaneStatus?
    var x86: LaneStatus?
}

struct LaneStatus: Codable, Sendable {
    var path: String?
    var exists: Bool?
    var executable: Bool?
}

struct DoctorGraphics: Codable, Sendable {
    var renderCore: Bool?
    var metalProbePass: Bool?
    var metalProbeStdout: String?

    enum CodingKeys: String, CodingKey {
        case renderCore = "render_core"
        case metalProbePass = "metal_probe_pass"
        case metalProbeStdout = "metal_probe_stdout"
    }
}

struct DoctorScripts: Codable, Sendable {
    var runWindowsApp: LaneStatus?
    var d3dSmoke: LaneStatus?

    enum CodingKeys: String, CodingKey {
        case runWindowsApp = "run_windows_app"
        case d3dSmoke = "d3d_smoke"
    }
}
