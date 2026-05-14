import SwiftUI

struct LocalTrialWizardView: View {
    @EnvironmentObject var settingsVM: SettingsViewModel
    @StateObject private var vm: LocalTrialWizardViewModel
    @Environment(\.dismiss) private var dismiss

    init() {
        self._vm = StateObject(wrappedValue: LocalTrialWizardViewModel(settings: SettingsViewModel().settings))
    }

    var body: some View {
        VStack(spacing: 0) {
            StepBar(steps: LocalTrialWizardViewModel.Step.allCases.map { $0.title }, current: vm.currentStep.rawValue)
                .padding()

            Divider()

            Group {
                switch vm.currentStep {
                case .select:
                    SelectAppStep(vm: vm)
                case .configure:
                    ConfigureStep(vm: vm)
                case .run:
                    RunStep(vm: vm)
                case .results:
                    ResultsStep(vm: vm)
                case .save:
                    SaveStep(vm: vm, dismiss: dismiss)
                }
            }
            .padding()
            .frame(maxHeight: .infinity)

            Divider()

            HStack {
                if vm.currentStep != .select {
                    Button("Back") {
                        withAnimation {
                            vm.currentStep = LocalTrialWizardViewModel.Step(rawValue: vm.currentStep.rawValue - 1) ?? .select
                        }
                    }
                }
                Spacer()
                if vm.currentStep == .results {
                    Button("Save to Library") {
                        withAnimation {
                            vm.currentStep = .save
                        }
                    }
                    .disabled(!vm.canProceed)
                } else if vm.currentStep == .save {
                    Button("Finish") {
                        dismiss()
                    }
                    .keyboardShortcut(.defaultAction)
                } else if vm.currentStep == .run {
                    Button("Next") {
                        withAnimation {
                            vm.currentStep = .results
                        }
                    }
                    .disabled(vm.runner.isRunning)
                } else {
                    Button("Next") {
                        withAnimation {
                            vm.currentStep = LocalTrialWizardViewModel.Step(rawValue: vm.currentStep.rawValue + 1) ?? .save
                        }
                    }
                    .disabled(!vm.canProceed)
                    .keyboardShortcut(.defaultAction)
                }
            }
            .padding()
        }
        .frame(minWidth: 700, minHeight: 500)
        .onAppear {
            vm.settings = settingsVM.settings
            vm.d3dBackend = settingsVM.settings.defaultD3DBackend
            vm.timeout = "\(settingsVM.settings.defaultTimeout)"
            vm.debugMode = settingsVM.settings.enableDebugLogs
            vm.keepArtifacts = settingsVM.settings.keepArtifactsDefault
        }
    }
}

private struct SelectAppStep: View {
    @ObservedObject var vm: LocalTrialWizardViewModel

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Select an executable to trial")
                .font(.title2)
            HStack {
                TextField("Path to .exe", text: $vm.exePath)
                Button("Choose…") { vm.pickExe() }
            }
            if !vm.exePath.isEmpty {
                if FileManager.default.fileExists(atPath: vm.exePath) {
                    Label("File exists", systemImage: "checkmark.circle.fill")
                        .foregroundStyle(.green)
                } else {
                    Label("File not found", systemImage: "xmark.circle.fill")
                        .foregroundStyle(.red)
                }
            }
            TextField("App Name (optional)", text: $vm.appName)
            Spacer()
        }
    }
}

private struct ConfigureStep: View {
    @ObservedObject var vm: LocalTrialWizardViewModel

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Configure run settings")
                .font(.title2)
            Form {
                Picker("D3D Backend", selection: $vm.d3dBackend) {
                    Text("None").tag("none")
                    Text("Mock").tag("mock")
                    Text("Metal").tag("metal")
                }
                TextField("Timeout (s)", text: $vm.timeout)
                    .frame(width: 80)
                Toggle("Debug Mode", isOn: $vm.debugMode)
                Toggle("Keep Artifacts", isOn: $vm.keepArtifacts)
            }
            .formStyle(.grouped)
            Spacer()
        }
    }
}

private struct RunStep: View {
    @ObservedObject var vm: LocalTrialWizardViewModel

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Running app...")
                .font(.title2)
            HStack {
                Button("Run") { vm.run() }
                    .disabled(vm.runner.isRunning)
                Button("Cancel") { vm.runner.cancel() }
                    .disabled(!vm.runner.isRunning)
                if vm.runner.isRunning {
                    ProgressView()
                        .padding(.leading, 8)
                }
                Spacer()
                if let result = vm.lastResult {
                    StatusBadge(status: result.status)
                }
            }
            HStack(spacing: 0) {
                VStack(alignment: .leading) {
                    Text("Stdout").font(.headline)
                    LogTextView(text: vm.runner.stdoutBuffer)
                }
                Divider()
                VStack(alignment: .leading) {
                    Text("Stderr").font(.headline)
                    LogTextView(text: vm.runner.stderrBuffer)
                        .foregroundStyle(.red)
                }
            }
            .frame(maxHeight: .infinity)
        }
    }
}

private struct ResultsStep: View {
    @ObservedObject var vm: LocalTrialWizardViewModel

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Run Results")
                .font(.title2)
            if let result = vm.lastResult {
                HStack {
                    StatusBadge(status: result.status)
                    if let ms = result.durationMs {
                        Text("\(ms) ms")
                            .font(.caption)
                            .monospacedDigit()
                    }
                    Spacer()
                }
                if result.status != "PASS" {
                    FailureTriageView(result: result, exePath: vm.exePath)
                }
                if result.d3dEnabled == true || result.d3dStatus != nil {
                    InlineD3DArtifactsView(result: result)
                }
            } else {
                ContentUnavailableView("No results yet", systemImage: "play.circle")
                    .frame(maxHeight: .infinity)
            }
            Spacer()
        }
    }
}

private struct SaveStep: View {
    @ObservedObject var vm: LocalTrialWizardViewModel
    let dismiss: DismissAction

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Save to Library")
                .font(.title2)
            if vm.saveSuccess {
                HStack {
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundStyle(.green)
                    Text("Saved successfully!")
                    Spacer()
                }
                .padding()
                .background(Color(.controlBackgroundColor))
                .cornerRadius(8)
            } else {
                Button("Save App Entry") {
                    vm.saveToLibrary()
                }
                .disabled(vm.isSaving)
                if vm.isSaving {
                    ProgressView()
                        .padding(.leading, 8)
                }
                if let error = vm.saveError {
                    Text(error)
                        .foregroundStyle(.red)
                        .font(.caption)
                }
            }
            Spacer()
        }
    }
}
