import Foundation
import Testing
@testable import MacRunnerControlCenter

struct DebugBundleV2Tests {
    @Test @MainActor func exportV2Bundle() throws {
        let settings = AppSettings.default
        let fm = FileManager.default
        let tmp = fm.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try fm.createDirectory(at: tmp, withIntermediateDirectories: true)

        let launcher = tmp.appendingPathComponent("launcher.json")
        try Data("{}".utf8).write(to: launcher)

        let bundle = DebugBundleExporterV2.export(
            settings: settings,
            app: nil,
            result: nil,
            compatibility: nil,
            manifest: nil,
            options: DebugBundleExporterV2.Options()
        )
        #expect(bundle != nil)
        if let b = bundle {
            #expect(fm.fileExists(atPath: b.path))
            let manifest = b.appendingPathComponent("bundle_manifest.json")
            #expect(fm.fileExists(atPath: manifest.path))
            try? fm.removeItem(at: b)
        }
        try? fm.removeItem(at: tmp)
    }

    @Test @MainActor func optionsControlInclusion() throws {
        let settings = AppSettings.default
        let fm = FileManager.default
        let bundle = DebugBundleExporterV2.export(
            settings: settings,
            app: nil,
            result: nil,
            compatibility: nil,
            manifest: nil,
            options: DebugBundleExporterV2.Options(includeSystemInfo: false, includeGitDiff: false)
        )
        #expect(bundle != nil)
        if let b = bundle {
            let manifest = b.appendingPathComponent("bundle_manifest.json")
            if let data = try? Data(contentsOf: manifest),
               let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
                #expect(json["system_info"] == nil)
            }
            try? fm.removeItem(at: b)
        }
    }
}
