import CryptoKit
import Foundation

struct ProfileValidationIssue: Identifiable, Codable, Equatable {
    var id: String { "\(path):\(field):\(reason)" }
    var path: String
    var field: String
    var reason: String
}

struct ProfileValidationReport: Codable, Equatable {
    var path: String
    var isValid: Bool
    var issues: [ProfileValidationIssue]
}

struct StrictProfileValidator {
    static let requiredFields = ["id", "name", "category", "arch", "windows_version", "required_dlls", "verified_versions"]

    func validate(data: Data, path: String = "profile.json") -> ProfileValidationReport {
        guard let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            let issue = ProfileValidationIssue(path: path, field: "$", reason: "Profile is not a JSON object")
            return ProfileValidationReport(path: path, isValid: false, issues: [issue])
        }
        var issues: [ProfileValidationIssue] = []
        for field in Self.requiredFields where object[field] == nil {
            issues.append(ProfileValidationIssue(path: path, field: field, reason: "Missing required field"))
        }
        for field in ["id", "name", "category", "arch", "windows_version"] where object[field] != nil && !(object[field] is String) {
            issues.append(ProfileValidationIssue(path: path, field: field, reason: "Expected string"))
        }
        for field in ["required_dlls", "verified_versions"] where object[field] != nil && !(object[field] is [String]) {
            issues.append(ProfileValidationIssue(path: path, field: field, reason: "Expected string array"))
        }
        return ProfileValidationReport(path: path, isValid: issues.isEmpty, issues: issues)
    }

    func validateDirectory(_ url: URL) -> [ProfileValidationReport] {
        let files = ((try? FileManager.default.contentsOfDirectory(at: url, includingPropertiesForKeys: nil)) ?? [])
            .filter { $0.pathExtension == "json" && $0.lastPathComponent != "profile.schema.json" && !$0.lastPathComponent.contains("manual-apps") }
        return files.map { file in
            let data = (try? Data(contentsOf: file)) ?? Data()
            return validate(data: data, path: file.path)
        }
    }
}

struct PEFingerprint {
    func sha256(url: URL) throws -> String {
        let data = try Data(contentsOf: url)
        let digest = SHA256.hash(data: data)
        return digest.map { String(format: "%02x", $0) }.joined()
    }

    func match(hash: String, profilesRoot: URL) -> String? {
        let files = ((try? FileManager.default.contentsOfDirectory(at: profilesRoot, includingPropertiesForKeys: nil)) ?? [])
            .filter { $0.pathExtension == "json" }
        for file in files {
            guard let data = try? Data(contentsOf: file),
                  let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let fingerprints = object["fingerprints"] as? [String],
                  fingerprints.contains(hash)
            else { continue }
            return object["id"] as? String
        }
        return nil
    }
}
