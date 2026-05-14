import SwiftUI

@MainActor
final class LocalTrialWizardViewModel: ObservableObject {
    enum Step: Int, CaseIterable {
        case select = 0
        case configure
        case run
        case results
        case save

        var title: String {
            switch self {
            case .select: return "Select App"
            case .configure: return "Configure"
            case .run: return "Run"
            case .results: return "Results"
            case .save: return "Save"
            }
        }
    }

    @Published var currentStep: Step = .select
    @Published var selectedApp: AppEntry?
    @Published var exePath = ""
    @Published var appName = ""
    @Published var d3dBackend = "none"
    @Published var timeout = "45"
    @Published var debugMode = false
    @Published var keepArtifacts = false
    @Published var runner = CommandRunner()
    @Published var lastResult: LauncherResult?
    @Published var triage: FailureClassifier.Diagnosis?
    @Published var isSaving = false
    @Published var saveError: String?
    @Published var saveSuccess = false

    var settings: AppSettings

    init(settings: AppSettings) {
        self.settings = settings
    }

    var canProceed: Bool {
        switch currentStep {
        case .select:
            return !exePath.isEmpty && FileManager.default.fileExists(atPath: exePath)
        case .configure:
            return true
        case .run:
            return !runner.isRunning
        case .results:
            return true
        case .save:
            return true
        }
    }

    func pickExe() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        if panel.runModal() == .OK, let url = panel.url {
            exePath = url.path
            appName = url.lastPathComponent
        }
    }

    func run() {
        guard !exePath.isEmpty else { return }
        lastResult = nil
        triage = nil
        saveSuccess = false

        var entry = AppEntry.new(name: appName.isEmpty ? (exePath as NSString).lastPathComponent : appName, exePath: exePath)
        entry.d3dBackend = d3dBackend
        entry.timeout = Int(timeout) ?? 45

        let runVM = RunAppViewModel(settings: settings)
        runVM.onComplete = { [weak self] result in
            guard let self = self else { return }
            Task { @MainActor in
                self.lastResult = result
                if let result = result, result.status != "PASS" {
                    self.triage = FailureClassifier.classify(result: result)
                }
            }
        }
        runVM.run(app: entry)

        // Mirror runner state for UI
        self.runner = runVM.runner
    }

    func saveToLibrary() {
        guard let result = lastResult else { return }
        isSaving = true
        saveError = nil

        var entry = AppEntry.new(name: appName.isEmpty ? (exePath as NSString).lastPathComponent : appName, exePath: exePath)
        entry.d3dBackend = d3dBackend
        entry.timeout = Int(timeout) ?? 45
        entry.lastRunStatus = result.status
        entry.lastDurationMs = result.durationMs

        var apps = ConfigStore.shared.loadApps()
        if let existing = apps.first(where: { $0.exePath == exePath }) {
            var updated = existing
            updated.lastRunStatus = result.status
            updated.lastDurationMs = result.durationMs
            updated.updatedAt = Date()
            apps = apps.map { $0.id == updated.id ? updated : $0 }
        } else {
            apps.append(entry)
        }
        ConfigStore.shared.saveApps(apps)

        if let app = apps.first(where: { $0.exePath == exePath }) {
            CompatibilityStore.shared.update(from: app, result: result)
        }

        isSaving = false
        saveSuccess = true
    }

    func reset() {
        currentStep = .select
        selectedApp = nil
        exePath = ""
        appName = ""
        d3dBackend = settings.defaultD3DBackend
        timeout = "\(settings.defaultTimeout)"
        debugMode = settings.enableDebugLogs
        keepArtifacts = settings.keepArtifactsDefault
        lastResult = nil
        triage = nil
        saveSuccess = false
        saveError = nil
    }
}
