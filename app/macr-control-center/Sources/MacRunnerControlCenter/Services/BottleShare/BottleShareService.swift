import CryptoKit
import Foundation

struct BottleArchiveManifest: Codable, Equatable {
    var schemaVersion: Int = 1
    var bottleName: String
    var profileID: String?
    var createdAt: Date
    var includedLargeBinaries: Bool
    var fileCount: Int
    var sha256: String
}

struct BottleArchiveResult: Codable, Equatable {
    var archiveURL: URL
    var manifest: BottleArchiveManifest
    var sizeBytes: Int64
}

struct BottleShareService {
    func exportBottle(bottlePath: URL, destination: URL, passphrase: String, includeLargeBinaries: Bool = false, profileID: String? = nil) throws -> BottleArchiveResult {
        let files = try collectFiles(root: bottlePath, includeLargeBinaries: includeLargeBinaries)
        let payload = try JSONEncoder().encode(files.map { $0.path.replacingOccurrences(of: bottlePath.path, with: "") })
        let encrypted = try encrypt(payload, passphrase: passphrase)
        try encrypted.write(to: destination)
        let hash = sha256(encrypted)
        let manifest = BottleArchiveManifest(bottleName: bottlePath.lastPathComponent, profileID: profileID, createdAt: Date(), includedLargeBinaries: includeLargeBinaries, fileCount: files.count, sha256: hash)
        return BottleArchiveResult(archiveURL: destination, manifest: manifest, sizeBytes: Int64(encrypted.count))
    }

    func decryptArchive(_ url: URL, passphrase: String) throws -> Data {
        let data = try Data(contentsOf: url)
        return try decrypt(data, passphrase: passphrase)
    }

    func encrypt(_ data: Data, passphrase: String) throws -> Data {
        let key = symmetricKey(passphrase)
        let sealed = try AES.GCM.seal(data, using: key)
        guard let combined = sealed.combined else { throw BottleShareError.encryptionFailed }
        return combined
    }

    func decrypt(_ data: Data, passphrase: String) throws -> Data {
        let box = try AES.GCM.SealedBox(combined: data)
        return try AES.GCM.open(box, using: symmetricKey(passphrase))
    }

    private func collectFiles(root: URL, includeLargeBinaries: Bool) throws -> [URL] {
        guard let enumerator = FileManager.default.enumerator(at: root, includingPropertiesForKeys: [.isRegularFileKey]) else { return [] }
        return enumerator.compactMap { item in
            guard let url = item as? URL else { return nil }
            if !includeLargeBinaries && ["exe", "msi", "iso"].contains(url.pathExtension.lowercased()) { return nil }
            return url
        }
    }

    private func symmetricKey(_ passphrase: String) -> SymmetricKey {
        let digest = SHA256.hash(data: Data(passphrase.utf8))
        return SymmetricKey(data: Data(digest))
    }

    private func sha256(_ data: Data) -> String {
        SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }
}

enum BottleShareError: LocalizedError { case encryptionFailed }
