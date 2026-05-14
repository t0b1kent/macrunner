struct LauncherResult: Codable, Sendable, Equatable {
    var schemaVersion: Int?
    var exe: String?
    var exePath: String?
    var arch: String?
    var machine: String?
    var executionLane: String?
    var status: String?
    var rc: Int?
    var exitCode: Int?
    var stdout: String?
    var stdoutPath: String?
    var stderrPath: String?
    var stderrTail: String?
    var durationMs: Int?
    var timedOut: Bool?
    var timeout: Bool?
    var crashed: Bool?
    var cleanupOk: Bool?
    var leftoversCount: Int?
    var d3dEnabled: Bool?
    var d3dBackend: String?
    var d3dStatus: String?
    var d3dTracePath: String?
    var d3dIrPath: String?
    var d3dReportPath: String?
    var d3dPpmPath: String?
    var d3dOutputChecksum: String?
    var d3dNonBackgroundPixels: Int?
    var d3dUnsupportedCalls: Int?
    var d3dValidationErrors: [String]?
    var metalDeviceDetected: Bool?
    var args: [String]?
    var envOverrides: [String]?
    var workdir: String?
    var command: [String]?
    var error: String?
    var cleanup: LauncherCleanup?

    enum CodingKeys: String, CodingKey {
        case schemaVersion = "schema_version"
        case exe
        case exePath = "exe_path"
        case arch
        case machine
        case executionLane = "execution_lane"
        case status
        case rc
        case exitCode = "exit_code"
        case stdout
        case stdoutPath = "stdout_path"
        case stderrPath = "stderr_path"
        case stderrTail = "stderr_tail"
        case durationMs = "duration_ms"
        case timedOut = "timed_out"
        case timeout
        case crashed
        case cleanupOk = "cleanup_ok"
        case leftoversCount = "leftovers_count"
        case d3dEnabled = "d3d_enabled"
        case d3dBackend = "d3d_backend"
        case d3dStatus = "d3d_status"
        case d3dTracePath = "d3d_trace_path"
        case d3dIrPath = "d3d_ir_path"
        case d3dReportPath = "d3d_report_path"
        case d3dPpmPath = "d3d_ppm_path"
        case d3dOutputChecksum = "d3d_output_checksum"
        case d3dNonBackgroundPixels = "d3d_non_background_pixels"
        case d3dUnsupportedCalls = "d3d_unsupported_calls"
        case d3dValidationErrors = "d3d_validation_errors"
        case metalDeviceDetected = "metal_device_detected"
        case args
        case envOverrides = "env_overrides"
        case workdir
        case command
        case error
        case cleanup
    }
}

struct LauncherCleanup: Codable, Sendable, Equatable {
    var before: CleanupSnapshot?
    var after: CleanupSnapshot?
    var killSent: Int?
    var termSent: Int?
    var winetempDirsRemoved: Int?

    enum CodingKeys: String, CodingKey {
        case before
        case after
        case killSent = "kill_sent"
        case termSent = "term_sent"
        case winetempDirsRemoved = "winetemp_dirs_removed"
    }
}

struct CleanupSnapshot: Codable, Sendable, Equatable {
    var active: Int?
    var exiting: Int?
    var total: Int?
    var sample: [String]?
}
