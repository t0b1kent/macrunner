import Foundation

struct EngineEnv {
    var macRunnerRoot: String
    var bundleResourceURL: URL?

    init(settings: AppSettings, bundle: Bundle = .main) {
        self.macRunnerRoot = settings.macRunnerRoot
        self.bundleResourceURL = bundle.resourceURL
    }

    func engineRoot() -> URL {
        if let bundled = bundleResourceURL?.appendingPathComponent("engine", isDirectory: true), isUsableEngineRoot(bundled) {
            return bundled
        }
        return URL(fileURLWithPath: macRunnerRoot).appendingPathComponent("engine", isDirectory: true)
    }

    func hyperbridgeRoot() -> URL {
        if let bundled = bundleResourceURL?.appendingPathComponent("hyperbridge", isDirectory: true), isUsableHyperbridgeRoot(bundled) {
            return bundled
        }
        return URL(fileURLWithPath: macRunnerRoot).appendingPathComponent("engine/hyperbridge", isDirectory: true)
    }

    func processEnvironment() -> [String: String] {
        ["MACRUNNER_ROOT": macRunnerRoot, "MACRUNNER_ENGINE_ROOT": engineRoot().path, "MACRUNNER_HYPERBRIDGE_ROOT": hyperbridgeRoot().path]
    }

    private func isUsableEngineRoot(_ root: URL) -> Bool {
        let candidates = [
            "wine/dist-pure-arm64/bin/wine",
            "wine/dist/bin/wine",
        ]
        return candidates.contains { relative in
            FileManager.default.isExecutableFile(atPath: root.appendingPathComponent(relative).path)
        }
    }

    private func isUsableHyperbridgeRoot(_ root: URL) -> Bool {
        let candidates = [
            "libhyperbridge.a",
            "src/hb_runtime.c",
        ]
        return candidates.contains { relative in
            FileManager.default.fileExists(atPath: root.appendingPathComponent(relative).path)
        }
    }
}
