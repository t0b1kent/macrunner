import SwiftUI

@MainActor
final class ReleaseManagerViewModel: ObservableObject {
    @Published var releases: [ReleaseInfo] = []
    @Published var isExporting = false
    @Published var lastExportPath: String?
    @Published var exportError: String?
    @Published var selectedRelease: ReleaseInfo?
    @Published var releaseNotes = ""
    @Published var includeCompatibilityDB = true
    @Published var includePerformanceReport = true
    @Published var includeDebugBundles = false
    @Published var includeManifests = true

    var settings: AppSettings

    init(settings: AppSettings) {
        self.settings = settings
        scanReleases()
    }

    func scanReleases() {
        let dir = "\(settings.macRunnerRoot)/releases"
        let fm = FileManager.default
        guard let files = try? fm.contentsOfDirectory(atPath: dir) else {
            releases = []
            return
        }
        var list: [ReleaseInfo] = []
        for file in files where file.hasSuffix(".zip") {
            let path = "\(dir)/\(file)"
            let attrs = try? fm.attributesOfItem(atPath: path)
            let size = attrs?[.size] as? Int64 ?? 0
            let mod = attrs?[.modificationDate] as? Date ?? Date()
            list.append(ReleaseInfo(
                name: file,
                path: path,
                sizeBytes: size,
                createdAt: mod
            ))
        }
        releases = list.sorted { $0.createdAt > $1.createdAt }
    }

    func exportRelease() {
        isExporting = true
        exportError = nil
        lastExportPath = nil

        Task {
            let fm = FileManager.default
            let df = DateFormatter()
            df.dateFormat = "yyyyMMdd-HHmmss"
            let stamp = df.string(from: Date())
            let name = "MacRunner-Release-\(stamp)"
            let releasesDir = "\(settings.macRunnerRoot)/releases"
            let tmpDir = "\(releasesDir)/.tmp-\(stamp)"
            let zipPath = "\(releasesDir)/\(name).zip"

            do {
                try fm.createDirectory(atPath: tmpDir, withIntermediateDirectories: true)
                try fm.createDirectory(atPath: releasesDir, withIntermediateDirectories: true)

                var manifest: [String: Any] = [
                    "name": name,
                    "created_at": ISO8601DateFormatter().string(from: Date()),
                    "macrunner_root": settings.macRunnerRoot
                ]

                if includeCompatibilityDB {
                    let src = "\(settings.macRunnerRoot)/db/compatibility.json"
                    let dest = "\(tmpDir)/compatibility.json"
                    if fm.fileExists(atPath: src) {
                        try fm.copyItem(atPath: src, toPath: dest)
                        manifest["compatibility_db"] = true
                    }
                }

                if includePerformanceReport {
                    let src = "\(settings.macRunnerRoot)/reports/performance.json"
                    let dest = "\(tmpDir)/performance.json"
                    if fm.fileExists(atPath: src) {
                        try fm.copyItem(atPath: src, toPath: dest)
                        manifest["performance_report"] = true
                    }
                }

                if includeManifests {
                    let srcDir = "\(settings.macRunnerRoot)/tests/real-app-manifests"
                    let destDir = "\(tmpDir)/manifests"
                    try fm.createDirectory(atPath: destDir, withIntermediateDirectories: true)
                    if let files = try? fm.contentsOfDirectory(atPath: srcDir) {
                        var copied: [String] = []
                        for file in files where file.hasSuffix(".json") {
                            try fm.copyItem(atPath: "\(srcDir)/\(file)", toPath: "\(destDir)/\(file)")
                            copied.append(file)
                        }
                        manifest["manifests"] = copied
                    }
                }

                if includeDebugBundles {
                    let srcDir = "\(settings.macRunnerRoot)/artifacts/control-center"
                    let destDir = "\(tmpDir)/debug-bundles"
                    try fm.createDirectory(atPath: destDir, withIntermediateDirectories: true)
                    if let files = try? fm.contentsOfDirectory(atPath: srcDir) {
                        var copied: [String] = []
                        for file in files {
                            let src = "\(srcDir)/\(file)"
                            let dest = "\(destDir)/\(file)"
                            var isDir: ObjCBool = false
                            if fm.fileExists(atPath: src, isDirectory: &isDir), isDir.boolValue {
                                try fm.copyItem(atPath: src, toPath: dest)
                                copied.append(file)
                            }
                        }
                        manifest["debug_bundles"] = copied
                    }
                }

                if !releaseNotes.isEmpty {
                    let notesPath = "\(tmpDir)/RELEASE-NOTES.md"
                    try releaseNotes.write(toFile: notesPath, atomically: true, encoding: .utf8)
                    manifest["release_notes"] = true
                }

                let manifestData = try JSONSerialization.data(withJSONObject: manifest, options: .prettyPrinted)
                try manifestData.write(to: URL(fileURLWithPath: "\(tmpDir)/release-manifest.json"))

                let process = Process()
                process.executableURL = URL(fileURLWithPath: "/usr/bin/ditto")
                process.arguments = ["-c", "-k", "--keepParent", tmpDir, zipPath]
                try process.run()
                process.waitUntilExit()

                try fm.removeItem(atPath: tmpDir)

                await MainActor.run {
                    self.isExporting = false
                    self.lastExportPath = zipPath
                    self.scanReleases()
                }
            } catch {
                await MainActor.run {
                    self.isExporting = false
                    self.exportError = error.localizedDescription
                }
            }
        }
    }

    func deleteRelease(_ release: ReleaseInfo) {
        try? FileManager.default.removeItem(atPath: release.path)
        scanReleases()
    }

    func revealInFinder(_ release: ReleaseInfo) {
        NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: release.path)])
    }
}

struct ReleaseInfo: Identifiable, Sendable {
    let id = UUID()
    var name: String
    var path: String
    var sizeBytes: Int64
    var createdAt: Date
}
