import CryptoKit
import Foundation

struct BugReportManifest: Codable, Equatable {
    var title: String
    var createdAt: String
    var files: [String]
    var networkUpload: Bool

    enum CodingKeys: String, CodingKey {
        case title
        case createdAt = "created_at"
        case files
        case networkUpload = "network_upload"
    }
}

struct InternalBugReportService {
    func dryRun(settings: AppSettings = .default) throws -> (zip: URL, sha256: String) {
        let stamp = ISO8601DateFormatter().string(from: Date()).replacingOccurrences(of: ":", with: "-")
        let work = FileManager.default.temporaryDirectory.appendingPathComponent("macrunner-bug-report-\(stamp)", isDirectory: true)
        try FileManager.default.createDirectory(at: work, withIntermediateDirectories: true)
        let manifest = BugReportManifest(title: "Dry-run bug report", createdAt: stamp, files: ["manifest.json", "sanitised-wine-log.txt", "diagnostic.md"], networkUpload: false)
        try JSONEncoder.pretty.encode(manifest).write(to: work.appendingPathComponent("manifest.json"))
        try sanitise("/Users/\(NSUserName())/Library/Wine log line\nsecret@example.com\n", home: NSHomeDirectory()).write(to: work.appendingPathComponent("sanitised-wine-log.txt"), atomically: true, encoding: .utf8)
        try "# MacRunner Bug Report\n\nNetwork upload: disabled\nGitHub: disabled\nSupport delivery: user-controlled local file\n".write(to: work.appendingPathComponent("diagnostic.md"), atomically: true, encoding: .utf8)
        let zip = FileManager.default.temporaryDirectory.appendingPathComponent("bug-report-\(stamp).zip")
        try? FileManager.default.removeItem(at: zip)
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/ditto")
        process.arguments = ["-c", "-k", "--norsrc", "--keepParent", work.path, zip.path]
        try process.run()
        process.waitUntilExit()
        let data = try Data(contentsOf: zip)
        let sha = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        return (zip, sha)
    }

    func sanitise(_ text: String, home: String) -> String {
        var output = text.replacingOccurrences(of: home, with: "~")
        output = output.replacingOccurrences(of: #"[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}"#, with: "<redacted-email>", options: [.regularExpression, .caseInsensitive])
        return output
    }
}
