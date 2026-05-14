import Foundation

struct TelemetryEvent: Codable, Equatable {
    var kind: String
    var payload: [String: String]
}

struct TelemetryService {
    var crashEndpoint = URL(string: "https://crashes.macrunner.app/upload")!
    var compatEndpoint = URL(string: "https://events.macrunner.app/compat")!
    var session: URLSession = .shared

    func sanitize(_ text: String, home: String = NSHomeDirectory()) -> String {
        var output = text.replacingOccurrences(of: home, with: "~")
        output = output.replacingOccurrences(of: #"[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}"#, with: "<redacted-email>", options: [.regularExpression, .caseInsensitive])
        let user = NSUserName()
        if !user.isEmpty { output = output.replacingOccurrences(of: user, with: "<user>") }
        return output
    }

    func makeCompatEvent(program: String, profile: String, status: String, stderrTail: String) -> TelemetryEvent {
        TelemetryEvent(kind: "compat", payload: ["program": sanitize(program), "profile": profile, "status": status, "stderr_tail": sanitize(stderrTail)])
    }

    func upload(_ event: TelemetryEvent, enabled: Bool) async throws -> Bool {
        guard enabled else { return false }
        let endpoint = event.kind == "crash" ? crashEndpoint : compatEndpoint
        var request = URLRequest(url: endpoint)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "content-type")
        request.httpBody = try JSONEncoder().encode(event)
        let (_, response) = try await session.data(for: request)
        let code = (response as? HTTPURLResponse)?.statusCode ?? 0
        return (200..<300).contains(code) || code == 404
    }
}

struct CrashReporterController {
    func configure(enabled: Bool) -> String {
        enabled ? "PLCrashReporter upload path enabled" : "Telemetry disabled; crash upload suppressed"
    }
}
