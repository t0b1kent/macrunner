import Foundation

struct SteamKeyStore {
    static let service = "app.macrunner.steam"
    private let apiKeyAccount = "api-key"

    func saveAPIKey(_ key: String) throws { try KeychainSecretStore(service: Self.service).save(key, account: apiKeyAccount) }
    func loadAPIKey() -> String? { KeychainSecretStore(service: Self.service).load(account: apiKeyAccount) }
    func deleteAPIKey() throws { try KeychainSecretStore(service: Self.service).delete(account: apiKeyAccount) }
}
