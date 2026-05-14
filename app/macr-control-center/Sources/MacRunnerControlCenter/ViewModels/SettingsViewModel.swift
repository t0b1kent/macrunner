import SwiftUI

@MainActor
final class SettingsViewModel: ObservableObject {
    @Published var settings: AppSettings {
        didSet { ConfigStore.shared.saveSettings(settings) }
    }
    @Published var isValidRoot = true
    @Published var showResetConfirm = false
    @Published var validationMessage: String?

    init() {
        self.settings = ConfigStore.shared.loadSettings()
        validateRoot()
    }

    func validateRoot() {
        let path = settings.macRunnerRoot
        let fm = FileManager.default
        let script = "\(path)/scripts/run-windows-app.sh"
        isValidRoot = fm.fileExists(atPath: path) && fm.fileExists(atPath: script)
        validationMessage = isValidRoot ? nil : "Root must contain scripts/run-windows-app.sh"
    }

    func pickRoot() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        if panel.runModal() == .OK, let url = panel.url {
            settings.macRunnerRoot = url.path
            validateRoot()
        }
    }

    func pickArtifactsDirectory() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        if panel.runModal() == .OK, let url = panel.url {
            settings.artifactsDirectory = url.path
        }
    }

    func pickBottlesDirectory() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        if panel.runModal() == .OK, let url = panel.url {
            settings.bottlesDirectory = url.path
        }
    }

    func resetDefaults() {
        settings = .default
        ConfigStore.shared.saveSettings(settings)
        validateRoot()
    }

    func exportSettings() {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.json]
        panel.nameFieldStringValue = "macr-settings.json"
        if panel.runModal() == .OK, let url = panel.url,
           let data = try? JSONEncoder().encode(settings) {
            try? data.write(to: url)
        }
    }

    func importSettings() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.json]
        if panel.runModal() == .OK, let url = panel.url,
           let data = try? Data(contentsOf: url),
           let imported = try? JSONDecoder().decode(AppSettings.self, from: data) {
            settings = imported
            validateRoot()
        }
    }
}
