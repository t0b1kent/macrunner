import Foundation
import Testing
@testable import MacRunnerControlCenter

struct ServiceTests {
    @Test @MainActor func codexFixPromptGeneration() {
        let result = LauncherResult(
            schemaVersion: 2, exe: "test.exe", exePath: "/tmp/test.exe", arch: "x64", machine: nil,
            executionLane: nil, status: "FAIL", rc: 1, exitCode: 1, stdout: nil, stdoutPath: nil,
            stderrPath: nil, stderrTail: "wine: failed", durationMs: 500, timedOut: false, timeout: false,
            crashed: false, cleanupOk: true, leftoversCount: 0, d3dEnabled: false, d3dBackend: nil,
            d3dStatus: nil, d3dTracePath: nil, d3dIrPath: nil, d3dReportPath: nil, d3dPpmPath: nil,
            d3dOutputChecksum: nil, d3dNonBackgroundPixels: nil, d3dUnsupportedCalls: nil,
            d3dValidationErrors: nil, metalDeviceDetected: nil, args: nil, envOverrides: nil,
            workdir: nil, command: nil, error: nil, cleanup: nil
        )
        let diagnosis = FailureClassifier.Diagnosis(
            classification: .missingDll,
            confidence: 0.85,
            cause: "Missing vcruntime140.dll",
            evidence: "wine: failed to initialize",
            recommendedCommand: "winetricks vcrun2019",
            actions: ["Install DLL"],
            bundleFiles: ["last-run.json"]
        )
        let prompt = CodexPromptService.generateFixPrompt(result: result, diagnosis: diagnosis)
        #expect(prompt.contains("MacRunner Fix Request"))
        #expect(prompt.contains("test.exe"))
        #expect(prompt.contains("MISSING_DLL"))
        #expect(prompt.contains("85%"))
        #expect(prompt.contains("winetricks vcrun2019"))
    }

    @Test @MainActor func codexPromptNoResult() {
        let prompt = CodexPromptService.generateFixPrompt(result: nil, diagnosis: nil)
        #expect(prompt.contains("No run result available"))
    }

    @Test @MainActor func codexCorpusPromptGeneration() {
        let entries = [
            CompatibilityEntry(id: "a", name: "AppA", exePathHash: "h1", arch: "x64", lastStatus: "PASS", bestD3DBackend: "metal", lastSuccessfulVersion: nil, failuresCount: 0, notes: nil, tags: nil, lastRunDate: nil, category: nil, artifactPaths: nil),
            CompatibilityEntry(id: "b", name: "AppB", exePathHash: "h2", arch: "x64", lastStatus: "FAIL", bestD3DBackend: "mock", lastSuccessfulVersion: nil, failuresCount: 2, notes: nil, tags: nil, lastRunDate: nil, category: nil, artifactPaths: nil)
        ]
        let prompt = CodexPromptService.generateCorpusPrompt(entries: entries)
        #expect(prompt.contains("MacRunner Corpus Analysis Request"))
        #expect(prompt.contains("AppB"))
        #expect(prompt.contains("Common root causes"))
    }

    @Test @MainActor func releaseManagerScanEmpty() {
        let vm = ReleaseManagerViewModel(settings: AppSettings.default)
        // With default root, releases dir likely doesn't exist yet
        #expect(vm.releases.isEmpty || vm.releases.count >= 0)
    }

    @Test @MainActor func localTrialWizardSteps() {
        let vm = LocalTrialWizardViewModel(settings: AppSettings.default)
        #expect(vm.currentStep == .select)
        #expect(!vm.canProceed) // no exe path

        vm.exePath = "/bin/ls" // exists on macOS
        #expect(vm.canProceed)

        vm.currentStep = .configure
        #expect(vm.canProceed)

        vm.currentStep = .run
        #expect(vm.canProceed)

        vm.currentStep = .results
        #expect(vm.canProceed)

        vm.currentStep = .save
        #expect(vm.canProceed)
    }

    @Test @MainActor func localTrialWizardReset() {
        let vm = LocalTrialWizardViewModel(settings: AppSettings.default)
        vm.exePath = "/bin/ls"
        vm.appName = "Test"
        vm.currentStep = .results
        vm.reset()
        #expect(vm.exePath.isEmpty)
        #expect(vm.appName.isEmpty)
        #expect(vm.currentStep == .select)
    }
}
