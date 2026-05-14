import CryptoKit
import Foundation

struct LicenseState: Codable, Equatable {
    var isLicensed: Bool
    var isTrial: Bool
    var daysRemaining: Int
    var message: String
}

struct FeatureGates: Codable, Equatable {
    var schemaVersion: Int
    var trialDays: Int
    var gatedFeatures: [String: Bool]

    enum CodingKeys: String, CodingKey {
        case schemaVersion = "schema_version"
        case trialDays = "trial_days"
        case gatedFeatures = "gated_features"
    }

    static let fallback = FeatureGates(schemaVersion: 1, trialDays: 14, gatedFeatures: ["ai_troubleshoot": true, "steam_library": true])
}

struct LicenseStore {
    static let service = "app.macrunner.license"
    private let account = "license-key"
    private let firstRunAccount = "first-run"

    func activate(key: String) throws -> LicenseState {
        let verifier = LicenseVerifier()
        if !verifier.isWellFormed(key) {
            _ = try verifier.verifyLicenseString(key)
        }
        try KeychainSecretStore(service: Self.service).save(key, account: account)
        return LicenseState(isLicensed: true, isTrial: false, daysRemaining: 0, message: "License activated")
    }

    func currentState(now: Date = Date()) -> LicenseState {
        if let key = KeychainSecretStore(service: Self.service).load(account: account) {
            let verifier = LicenseVerifier()
            if verifier.isWellFormed(key) || ((try? verifier.verifyLicenseString(key, now: now)) != nil) {
                return LicenseState(isLicensed: true, isTrial: false, daysRemaining: 0, message: "Licensed")
            }
        }
        let firstRun = loadFirstRun(now: now)
        let days = max(0, 14 - Calendar.current.dateComponents([.day], from: firstRun, to: now).day!)
        return LicenseState(isLicensed: false, isTrial: days > 0, daysRemaining: days, message: days > 0 ? "Trial Mode" : "Trial expired")
    }

    func isFeatureAllowed(_ feature: String, now: Date = Date()) -> Bool {
        let gates = loadFeatureGates()
        guard gates.gatedFeatures[feature] == true else { return true }
        let state = currentState(now: now)
        return state.isLicensed || state.isTrial
    }

    func loadFeatureGates() -> FeatureGates {
        let url = URL(fileURLWithPath: AppSettings.defaultRoot).appendingPathComponent("licensing/feature-gates.json")
        guard let data = try? Data(contentsOf: url), let gates = try? JSONDecoder().decode(FeatureGates.self, from: data) else { return .fallback }
        return gates
    }

    private func loadFirstRun(now: Date) -> Date {
        let store = KeychainSecretStore(service: Self.service)
        if let text = store.load(account: firstRunAccount), let seconds = TimeInterval(text) {
            return Date(timeIntervalSince1970: seconds)
        }
        try? store.save(String(now.timeIntervalSince1970), account: firstRunAccount)
        return now
    }
}

struct LicenseVerificationResult: Equatable {
    var email: String
    var expiresAt: Date
    var expiresPrefix: String
}

struct LicenseVerifier {
    private static let publicKeyPEM = """
    -----BEGIN PUBLIC KEY-----
    MCowBQYDK2VwAyEAI7iSWJk4bgll6KPRr+2D4Y+TXeNkCn8gQwTWhOlFrWg=
    -----END PUBLIC KEY-----
    """

    func isWellFormed(_ key: String) -> Bool {
        let parts = key.split(separator: "-")
        return parts.count == 4 && parts.allSatisfy { $0.count == 5 && $0.allSatisfy { $0.isLetter || $0.isNumber } }
    }

    func verifyLicenseString(_ license: String, now: Date = Date()) throws -> LicenseVerificationResult {
        let parts = license.split(separator: "|", omittingEmptySubsequences: false).map(String.init)
        guard parts.count == 3, !parts[0].isEmpty, !parts[1].isEmpty, !parts[2].isEmpty else {
            throw LicenseError.invalidFormat
        }

        let payload = "\(parts[0])|\(parts[1])"
        guard let expires = ISO8601DateFormatter().date(from: parts[1]) else {
            throw LicenseError.invalidFormat
        }
        guard expires > now else { throw LicenseError.expired }

        let signature = try Data(hexEncoded: parts[2])
        guard signature.count == 64 else { throw LicenseError.invalidFormat }
        let publicKey = try publicKeyRawRepresentation(fromPEM: Self.publicKeyPEM)
        guard verifyDetachedSignature(message: Data(payload.utf8), signature: signature, publicKeyRaw: publicKey) else {
            throw LicenseError.invalidSignature
        }

        return LicenseVerificationResult(email: parts[0], expiresAt: expires, expiresPrefix: parts[1])
    }

    func verifyDetachedSignature(message: Data, signature: Data, publicKeyRaw: Data) -> Bool {
        guard let key = try? Curve25519.Signing.PublicKey(rawRepresentation: publicKeyRaw) else { return false }
        return key.isValidSignature(signature, for: message)
    }

    private func publicKeyRawRepresentation(fromPEM pem: String) throws -> Data {
        let body = pem
            .split(separator: "\n")
            .filter { !$0.contains("BEGIN PUBLIC KEY") && !$0.contains("END PUBLIC KEY") }
            .joined()
        guard let der = Data(base64Encoded: body) else { throw LicenseError.invalidPublicKey }
        if der.count == 32 { return der }
        guard der.count > 32 else { throw LicenseError.invalidPublicKey }
        return der.suffix(32)
    }
}

enum LicenseError: LocalizedError, Equatable {
    case invalidFormat
    case invalidSignature
    case expired
    case invalidPublicKey

    var errorDescription: String? {
        switch self {
        case .invalidFormat:
            return "License must be either 4 blocks of 5 alphanumeric characters or email|expires_iso8601|signature_hex."
        case .invalidSignature:
            return "License signature is invalid."
        case .expired:
            return "License is expired."
        case .invalidPublicKey:
            return "Bundled license public key is invalid."
        }
    }
}

private extension Data {
    init(hexEncoded hex: String) throws {
        guard hex.count.isMultiple(of: 2) else { throw LicenseError.invalidFormat }
        var bytes = [UInt8]()
        bytes.reserveCapacity(hex.count / 2)
        var index = hex.startIndex
        while index < hex.endIndex {
            let next = hex.index(index, offsetBy: 2)
            guard let byte = UInt8(hex[index..<next], radix: 16) else { throw LicenseError.invalidFormat }
            bytes.append(byte)
            index = next
        }
        self = Data(bytes)
    }
}
