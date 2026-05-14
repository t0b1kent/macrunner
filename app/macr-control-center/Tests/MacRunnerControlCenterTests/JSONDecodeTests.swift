import Foundation
import Testing
@testable import MacRunnerControlCenter

struct JSONDecodeTests {
    private var fixturesDir: URL {
        URL(fileURLWithPath: AppSettings.defaultRoot).appendingPathComponent("app/macr-control-center/Fixtures")
    }

    @Test func decodeLauncherSuccess() throws {
        let url = fixturesDir.appendingPathComponent("launcher_success.json")
        let data = try Data(contentsOf: url)
        let result = try JSONDecoder().decode(LauncherResult.self, from: data)
        #expect(result.status == "PASS")
        #expect(result.rc == 0)
        #expect(result.d3dEnabled == false)
    }

    @Test func decodeLauncherCrash() throws {
        let url = fixturesDir.appendingPathComponent("launcher_crash.json")
        let data = try Data(contentsOf: url)
        let result = try JSONDecoder().decode(LauncherResult.self, from: data)
        #expect(result.status == "CRASH")
        #expect(result.crashed == true)
    }

    @Test func decodeLauncherD3D() throws {
        let url = fixturesDir.appendingPathComponent("launcher_d3d_success.json")
        let data = try Data(contentsOf: url)
        let result = try JSONDecoder().decode(LauncherResult.self, from: data)
        #expect(result.d3dEnabled == true)
        #expect(result.d3dBackend == "mock")
        #expect(result.d3dNonBackgroundPixels == 1152)
    }

    @Test func decodeDoctor() throws {
        let url = fixturesDir.appendingPathComponent("doctor_success.json")
        let data = try Data(contentsOf: url)
        let report = try JSONDecoder().decode(DoctorReport.self, from: data)
        #expect(report.host?.macos == "26.4")
        #expect(report.graphics?.metalProbePass == true)
    }

    @Test func decodePlatformVerify() throws {
        let url = fixturesDir.appendingPathComponent("platform_verify_success.json")
        let data = try Data(contentsOf: url)
        let report = try JSONDecoder().decode(PlatformVerifyReport.self, from: data)
        #expect(report.status == "PASS")
    }

    @Test func invalidJSONShowsErrorGracefully() {
        let bad = Data("not json".utf8)
        let decoded = try? JSONDecoder().decode(LauncherResult.self, from: bad)
        #expect(decoded == nil)
    }

    @Test func decodeD3DReportInline() throws {
        let json = """
        {
            "status": "FAIL",
            "backend": "mock",
            "non_background_pixels": 0,
            "unsupported_calls": 5,
            "validation_errors": ["err1", "err2"],
            "metal_device_detected": false
        }
        """
        let data = Data(json.utf8)
        let report = try JSONDecoder().decode(D3DReport.self, from: data)
        #expect(report.status == "FAIL")
        #expect(report.unsupportedCalls == 5)
        #expect(report.validationErrors?.count == 2)
        #expect(report.metalDeviceDetected == false)
    }
}
