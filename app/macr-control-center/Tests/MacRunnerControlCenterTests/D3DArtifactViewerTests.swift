import Foundation
import Testing
@testable import MacRunnerControlCenter

struct D3DArtifactViewerTests {
    @Test func parseTraceJsonl() {
        let lines = [
            "{\"ts\":\"2026-05-12T09:30:01Z\",\"call\":\"D3D11CreateDevice\",\"rc\":0}",
            "{\"ts\":\"2026-05-12T09:30:02Z\",\"call\":\"Present\",\"rc\":0}"
        ]
        let parsed = lines.compactMap { line -> TraceEvent? in
            guard let data = line.data(using: .utf8),
                  let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
            return TraceEvent(ts: json["ts"] as? String ?? "", call: json["call"] as? String ?? "", rc: json["rc"] as? Int ?? 0)
        }
        #expect(parsed.count == 2)
        #expect(parsed[0].call == "D3D11CreateDevice")
    }

    @Test func parseSampleRuntimeReport() throws {
        let url = URL(fileURLWithPath: AppSettings.defaultRoot).appendingPathComponent("app/macr-control-center/Fixtures/sample_runtime_report.json")
        let data = try Data(contentsOf: url)
        let report = try JSONDecoder().decode(D3DReport.self, from: data)
        #expect(report.status == "PASS")
        #expect(report.nonBackgroundPixels == 1152)
    }
}
