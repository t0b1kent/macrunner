import CryptoKit
import Foundation

struct ManualImportResult: Codable, Equatable {
    var bottleID: String
    var profileIDOrNull: String?
    var suggestedName: String
    var executablePath: String
    var peHash: String
    var arch: String

    enum CodingKeys: String, CodingKey {
        case bottleID = "bottle_id"
        case profileIDOrNull = "profile_id_or_null"
        case suggestedName = "suggested_name"
        case executablePath = "executable_path"
        case peHash = "pe_hash"
        case arch
    }
}

struct ManualImportWizard {
    func importExecutable(path: String, settings: AppSettings = .default) throws -> ManualImportResult {
        let url = URL(fileURLWithPath: path)
        let data = try Data(contentsOf: url)
        let hash = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        let arch = detectPEArch(data: data)
        let name = url.deletingPathExtension().lastPathComponent.isEmpty ? "Windows Program" : url.deletingPathExtension().lastPathComponent
        let profile: String? = arch == "x86_64" ? "generic-x86_64-rosetta" : (arch == "x86" ? "generic-x86" : "generic-arm64")
        let bottleID = safe("manual-\(name)-\(hash.prefix(8))")
        let bottleURL = URL(fileURLWithPath: settings.bottlesDirectory).appendingPathComponent(bottleID, isDirectory: true)
        try FileManager.default.createDirectory(at: bottleURL, withIntermediateDirectories: true)
        let manifest: [String: Any] = ["bottle_id": bottleID, "name": name, "exe": path, "profile": profile as Any, "pe_hash": hash, "arch": arch]
        let json = try JSONSerialization.data(withJSONObject: manifest, options: [.prettyPrinted, .sortedKeys])
        try json.write(to: bottleURL.appendingPathComponent("bottle.json"))
        return ManualImportResult(bottleID: bottleID, profileIDOrNull: profile, suggestedName: name, executablePath: path, peHash: hash, arch: arch)
    }

    private func detectPEArch(data: Data) -> String {
        guard data.count > 0x40, data[0] == 0x4d, data[1] == 0x5a else { return "unknown" }
        let peOffset = Int(data[0x3c]) | Int(data[0x3d]) << 8 | Int(data[0x3e]) << 16 | Int(data[0x3f]) << 24
        guard data.count > peOffset + 6, data[peOffset] == 0x50, data[peOffset + 1] == 0x45 else { return "unknown" }
        let machine = UInt16(data[peOffset + 4]) | UInt16(data[peOffset + 5]) << 8
        switch machine {
        case 0x8664: return "x86_64"
        case 0x014c: return "x86"
        case 0xaa64: return "arm64"
        default: return String(format: "machine-0x%04x", machine)
        }
    }

    private func safe(_ text: String) -> String {
        text.lowercased().map { $0.isLetter || $0.isNumber || $0 == "-" ? String($0) : "-" }.joined()
    }
}
