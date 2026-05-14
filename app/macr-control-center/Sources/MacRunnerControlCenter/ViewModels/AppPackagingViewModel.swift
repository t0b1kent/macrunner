import SwiftUI

@MainActor
final class AppPackagingViewModel: ObservableObject {
    @Published var apps: [AppEntry] = []
    @Published var selectedApp: AppEntry?
    @Published var bundleName = ""
    @Published var outputPath = ""
    @Published var isPackaging = false
    @Published var lastResult: String?

    var settings: AppSettings

    init(settings: AppSettings) {
        self.settings = settings
        self.outputPath = NSSearchPathForDirectoriesInDomains(.desktopDirectory, .userDomainMask, true).first ?? ""
    }

    func refresh() {
        apps = ConfigStore.shared.loadApps()
        if let app = selectedApp, !apps.contains(where: { $0.id == app.id }) {
            selectedApp = nil
        }
    }

    func pickOutputDirectory() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        if panel.runModal() == .OK, let url = panel.url {
            outputPath = url.path
        }
    }

    func package() {
        guard let app = selectedApp else { return }
        let name = bundleName.isEmpty ? app.name : bundleName
        let dest = "\(outputPath)/\(name).app"
        isPackaging = true
        lastResult = nil

        Task {
            let fm = FileManager.default
            let contents = "\(dest)/Contents"
            let macOS = "\(contents)/MacOS"
            let resources = "\(contents)/Resources"

            do {
                try fm.createDirectory(atPath: macOS, withIntermediateDirectories: true)
                try fm.createDirectory(atPath: resources, withIntermediateDirectories: true)

                let plist: [String: Any] = [
                    "CFBundleExecutable": name,
                    "CFBundleIdentifier": "com.macrunner.\(app.id.uuidString)",
                    "CFBundleName": name,
                    "CFBundleVersion": "1.0",
                    "CFBundlePackageType": "APPL",
                    "LSMinimumSystemVersion": "14.0"
                ]
                let plistData = try PropertyListSerialization.data(fromPropertyList: plist, format: .xml, options: 0)
                try plistData.write(to: URL(fileURLWithPath: "\(contents)/Info.plist"))

                let script = """
                #!/bin/bash
                set -e
                ROOT="\(settings.macRunnerRoot)"
                EXE="\(app.exePath)"
                cd "$ROOT"
                "$ROOT/scripts/run-windows-app.sh" "$EXE"
                """
                let scriptPath = "\(macOS)/\(name)"
                try script.write(toFile: scriptPath, atomically: true, encoding: .utf8)
                try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: scriptPath)

                await MainActor.run {
                    self.isPackaging = false
                    self.lastResult = "Packaged to \(dest)"
                }
            } catch {
                await MainActor.run {
                    self.isPackaging = false
                    self.lastResult = "Failed: \(error.localizedDescription)"
                }
            }
        }
    }
}
