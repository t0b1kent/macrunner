struct D3DReport: Codable, Sendable {
    var status: String?
    var backend: String?
    var tracePath: String?
    var reportPath: String?
    var ppmPath: String?
    var irPath: String?
    var checksum: String?
    var nonBackgroundPixels: Int?
    var unsupportedCalls: Int?
    var validationErrors: [String]?
    var metalDeviceDetected: Bool?

    enum CodingKeys: String, CodingKey {
        case status
        case backend
        case tracePath = "trace_path"
        case reportPath = "report_path"
        case ppmPath = "ppm_path"
        case irPath = "ir_path"
        case checksum
        case nonBackgroundPixels = "non_background_pixels"
        case unsupportedCalls = "unsupported_calls"
        case validationErrors = "validation_errors"
        case metalDeviceDetected = "metal_device_detected"
    }
}
