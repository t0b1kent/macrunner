import Foundation

enum ControlCenterCLI {
    static func handleIfNeeded(arguments: [String] = CommandLine.arguments) -> Bool {
        guard arguments.count > 1 else { return false }
        switch arguments[1] {
        case "--print-tool-paths":
            let paths = StoreToolLocator.toolPaths()
            print("legendary=\(paths["legendary"] ?? "missing")")
            print("gogdl=\(paths["gogdl"] ?? "missing")")
            exit(0)
        case "--hud-synthetic":
            do {
                let count = try SyntheticPerfEmitter().run()
                print("wrote \(count) samples to \(AppSettings.defaultRoot)/run/hud.sock.log")
                exit(0)
            } catch {
                fputs("hud synthetic failed: \(error.localizedDescription)\n", stderr)
                exit(1)
            }
        case "--verify-license":
            let license = arguments.dropFirst(2).joined(separator: " ")
            do {
                let result = try LicenseVerifier().verifyLicenseString(license)
                print("OK, valid until \(result.expiresPrefix)")
                exit(0)
            } catch {
                fputs("license verification failed: \(licenseErrorMessage(error))\n", stderr)
                exit(1)
            }
        default:
            return false
        }
    }

    private static func licenseErrorMessage(_ error: Error) -> String {
        guard let licenseError = error as? LicenseError else { return error.localizedDescription }
        switch licenseError {
        case .invalidFormat, .invalidSignature:
            return "signature mismatch"
        case .expired, .invalidPublicKey:
            return licenseError.localizedDescription
        }
    }
}
