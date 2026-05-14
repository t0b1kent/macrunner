import Foundation
import Testing
@testable import MacRunnerControlCenter

struct DebugBundleTests {
    @Test @MainActor func exportBundle() throws {
        let settings = AppSettings.default
        let fm = FileManager.default
        let tmp = fm.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try fm.createDirectory(at: tmp, withIntermediateDirectories: true)

        let launcher = tmp.appendingPathComponent("launcher.json")
        try Data("{}".utf8).write(to: launcher)

        let bundle = DebugBundleExporter.export(
            settings: settings,
            lastLauncherPath: launcher.path,
            includeFullLogs: false
        )
        #expect(bundle != nil)
        if let b = bundle {
            #expect(fm.fileExists(atPath: b.path))
            try? fm.removeItem(at: b)
        }
        try? fm.removeItem(at: tmp)
    }

    @Test @MainActor func skipsMissingFiles() throws {
        let settings = AppSettings.default
        let fm = FileManager.default
        let bundle = DebugBundleExporter.export(
            settings: settings,
            lastLauncherPath: "/nonexistent/path.json",
            includeFullLogs: false
        )
        #expect(bundle != nil)
        if let b = bundle {
            let manifest = b.appendingPathComponent("manifest.json")
            #expect(fm.fileExists(atPath: manifest.path))
            try? fm.removeItem(at: b)
        }
    }
}
