import Foundation

struct EngineEnv {
    var macRunnerRoot: String
    var bundleResourceURL: URL?

    init(settings: AppSettings, bundle: Bundle = .main) {
        self.macRunnerRoot = settings.macRunnerRoot
        self.bundleResourceURL = bundle.resourceURL
    }

    func engineRoot() -> URL {
        if let bundled = bundleResourceURL?.appendingPathComponent("engine", isDirectory: true), FileManager.default.fileExists(atPath: bundled.path) {
            return bundled
        }
        return URL(fileURLWithPath: macRunnerRoot).appendingPathComponent("engine", isDirectory: true)
    }

    func hyperbridgeRoot() -> URL {
        if let bundled = bundleResourceURL?.appendingPathComponent("hyperbridge", isDirectory: true), FileManager.default.fileExists(atPath: bundled.path) {
            return bundled
        }
        return URL(fileURLWithPath: macRunnerRoot).appendingPathComponent("engine/hyperbridge", isDirectory: true)
    }

    func processEnvironment() -> [String: String] {
        ["MACRUNNER_ROOT": macRunnerRoot, "MACRUNNER_ENGINE_ROOT": engineRoot().path, "MACRUNNER_HYPERBRIDGE_ROOT": hyperbridgeRoot().path]
    }
}
