import SwiftUI

struct OnboardingView: View {
    @Binding var isPresented: Bool
    @EnvironmentObject var settingsVM: SettingsViewModel
    @State private var step = 0
    @State private var rootPath = AppSettings.defaultRoot
    @State private var doctorOutput = ""
    @State private var doctorOK = false
    @State private var verifyOutput = ""
    @State private var verifyOK = false
    @State private var isRunningCheck = false
    @State private var errorText: String?

    private let steps = ["Welcome", "Doctor", "Verify", "Settings", "Done"]

    var body: some View {
        VStack(spacing: 16) {
            StepBar(steps: steps, current: step)
                .padding(.horizontal)

            if step == 0 {
                welcomeStep
            } else if step == 1 {
                doctorStep
            } else if step == 2 {
                verifyStep
            } else if step == 3 {
                settingsStep
            } else if step == 4 {
                doneStep
            }
        }
        .padding()
        .frame(minWidth: 560, minHeight: 420)
    }

    var welcomeStep: some View {
        VStack(spacing: 16) {
            Text("Welcome to MacRunner Control Center")
                .font(.title)
            Text("Let's set up your environment.")
                .foregroundStyle(.secondary)

            HStack {
                TextField("MacRunner Root", text: $rootPath)
                    .frame(width: 360)
                Button("Browse...") {
                    let panel = NSOpenPanel()
                    panel.canChooseDirectories = true
                    panel.canChooseFiles = false
                    if panel.runModal() == .OK, let url = panel.url {
                        rootPath = url.path
                    }
                }
            }

            if let error = errorText {
                Label(error, systemImage: "exclamationmark.triangle")
                    .foregroundStyle(.orange)
                    .font(.callout)
            }

            HStack {
                Spacer()
                Button("Next") {
                    validateRoot()
                }
                .keyboardShortcut(.defaultAction)
            }
        }
    }

    var doctorStep: some View {
        VStack(spacing: 12) {
            Text("Doctor Check").font(.title)
            if doctorOutput.isEmpty {
                Button("Run Doctor") { runDoctor() }
                    .disabled(isRunningCheck)
                if isRunningCheck {
                    ProgressView("Running doctor...")
                }
            } else {
                ScrollView {
                    Text(doctorOutput)
                        .font(.system(.caption, design: .monospaced))
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .textSelection(.enabled)
                }
                .frame(height: 200)
                .background(Color(.textBackgroundColor))
                .cornerRadius(8)

                HStack {
                    Image(systemName: doctorOK ? "checkmark.circle.fill" : "xmark.circle.fill")
                        .foregroundStyle(doctorOK ? .green : .red)
                    Text(doctorOK ? "Doctor passed" : "Doctor reported issues")
                        .foregroundStyle(doctorOK ? .green : .red)
                    Spacer()
                }

                HStack {
                    Button("Re-run") { runDoctor() }
                    Spacer()
                    Button("Next") { step = 2 }
                        .keyboardShortcut(.defaultAction)
                }
            }
        }
    }

    var verifyStep: some View {
        VStack(spacing: 12) {
            Text("Platform Verify").font(.title)
            if verifyOutput.isEmpty {
                Button("Run Verify") { runVerify() }
                    .disabled(isRunningCheck)
                if isRunningCheck {
                    ProgressView("Running verify...")
                }
            } else {
                ScrollView {
                    Text(verifyOutput)
                        .font(.system(.caption, design: .monospaced))
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .textSelection(.enabled)
                }
                .frame(height: 200)
                .background(Color(.textBackgroundColor))
                .cornerRadius(8)

                HStack {
                    Image(systemName: verifyOK ? "checkmark.circle.fill" : "xmark.circle.fill")
                        .foregroundStyle(verifyOK ? .green : .red)
                    Text(verifyOK ? "Verify passed" : "Verify reported issues")
                        .foregroundStyle(verifyOK ? .green : .red)
                    Spacer()
                }

                HStack {
                    Button("Back") { step = 1 }
                    Button("Re-run") { runVerify() }
                    Spacer()
                    Button("Next") { step = 3 }
                        .keyboardShortcut(.defaultAction)
                }
            }
        }
    }

    var settingsStep: some View {
        VStack(spacing: 12) {
            Text("Default Settings").font(.title)
            Form {
                TextField("Artifacts Directory", text: $settingsVM.settings.artifactsDirectory)
                TextField("Bottles Directory", text: $settingsVM.settings.bottlesDirectory)
                Picker("Default D3D Backend", selection: $settingsVM.settings.defaultD3DBackend) {
                    Text("None").tag("none")
                    Text("Mock").tag("mock")
                    Text("Metal").tag("metal")
                }
                TextField("Default Timeout", value: $settingsVM.settings.defaultTimeout, format: .number)
                TextField("Doctor Timeout", value: $settingsVM.settings.doctorTimeout, format: .number)
                TextField("Integration Timeout", value: $settingsVM.settings.integrationTimeout, format: .number)
                Toggle("Keep Artifacts", isOn: $settingsVM.settings.keepArtifactsDefault)
                Toggle("Enable Debug Logs", isOn: $settingsVM.settings.enableDebugLogs)
                Toggle("Enable Mock Mode", isOn: $settingsVM.settings.enableMockMode)
            }
            .frame(width: 420)

            HStack {
                Button("Back") { step = 2 }
                Spacer()
                Button("Finish") {
                    ConfigStore.shared.saveSettings(settingsVM.settings)
                    step = 4
                }
                .keyboardShortcut(.defaultAction)
            }
        }
    }

    var doneStep: some View {
        VStack(spacing: 16) {
            Image(systemName: "checkmark.seal.fill")
                .font(.system(size: 48))
                .foregroundStyle(.green)
            Text("Setup Complete")
                .font(.title)
            Text("MacRunner Control Center is ready to use.")
                .foregroundStyle(.secondary)

            VStack(alignment: .leading, spacing: 4) {
                Text("Root: \(settingsVM.settings.macRunnerRoot)")
                Text("Backend: \(settingsVM.settings.defaultD3DBackend)")
                Text("Doctor: \(doctorOK ? "PASS" : "ISSUES")")
                Text("Verify: \(verifyOK ? "PASS" : "ISSUES")")
            }
            .font(.caption)
            .foregroundStyle(.secondary)

            Button("Open Control Center") {
                isPresented = false
            }
            .keyboardShortcut(.defaultAction)
        }
    }

    private func validateRoot() {
        settingsVM.settings.macRunnerRoot = rootPath
        settingsVM.validateRoot()
        if settingsVM.isValidRoot {
            errorText = nil
            step = 1
        } else {
            errorText = "Invalid root: scripts/run-windows-app.sh not found."
        }
    }

    private func runDoctor() {
        isRunningCheck = true
        doctorOutput = ""
        Task {
            let script = "\(rootPath)/scripts/macr-doctor.sh"
            let process = Process()
            process.executableURL = URL(fileURLWithPath: script)
            process.currentDirectoryURL = URL(fileURLWithPath: rootPath)
            let pipe = Pipe()
            process.standardOutput = pipe
            process.standardError = pipe
            do {
                try process.run()
                process.waitUntilExit()
                let data = pipe.fileHandleForReading.readDataToEndOfFile()
                let output = String(data: data, encoding: .utf8) ?? ""
                await MainActor.run {
                    self.doctorOutput = output
                    self.doctorOK = output.contains("PASS")
                    self.isRunningCheck = false
                }
            } catch {
                await MainActor.run {
                    self.doctorOutput = "Failed to run doctor: \(error.localizedDescription)"
                    self.doctorOK = false
                    self.isRunningCheck = false
                }
            }
        }
    }

    private func runVerify() {
        isRunningCheck = true
        verifyOutput = ""
        Task {
            let script = "\(rootPath)/scripts/verify-native-platform.sh"
            let process = Process()
            process.executableURL = URL(fileURLWithPath: script)
            process.currentDirectoryURL = URL(fileURLWithPath: rootPath)
            let pipe = Pipe()
            process.standardOutput = pipe
            process.standardError = pipe
            do {
                try process.run()
                process.waitUntilExit()
                let data = pipe.fileHandleForReading.readDataToEndOfFile()
                let output = String(data: data, encoding: .utf8) ?? ""
                await MainActor.run {
                    self.verifyOutput = output
                    self.verifyOK = output.contains("PASS")
                    self.isRunningCheck = false
                }
            } catch {
                await MainActor.run {
                    self.verifyOutput = "Failed to run verify: \(error.localizedDescription)"
                    self.verifyOK = false
                    self.isRunningCheck = false
                }
            }
        }
    }
}
