import SwiftUI

struct SettingsView: View {
    @EnvironmentObject var vm: SettingsViewModel

    @StateObject private var coreStatusVM = CoreStatusViewModel()
    @State private var anthropicAPIKey = ""
    @State private var steamAPIKey = ""
    @State private var licenseKey = ""
    @State private var keychainStatus = ""

    var body: some View {
        Form {
            Section("Core Connection") {
                CoreConnectionView(vm: coreStatusVM, rootPath: vm.settings.macRunnerRoot)
            }

            Section("Paths") {
                HStack {
                    TextField("MacRunner Root", text: $vm.settings.macRunnerRoot)
                    Button("Browse...") { vm.pickRoot() }
                    Image(systemName: vm.isValidRoot ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
                        .foregroundStyle(vm.isValidRoot ? .green : .orange)
                }
                if let msg = vm.validationMessage {
                    Text(msg)
                        .font(.caption)
                        .foregroundStyle(.orange)
                }
                HStack {
                    TextField("Artifacts Directory", text: $vm.settings.artifactsDirectory)
                    Button("Browse...") { vm.pickArtifactsDirectory() }
                }
                HStack {
                    TextField("Bottles Directory", text: $vm.settings.bottlesDirectory)
                    Button("Browse...") { vm.pickBottlesDirectory() }
                }
            }

            Section("Defaults") {
                TextField("Default Timeout", value: $vm.settings.defaultTimeout, format: .number)
                Picker("Default D3D Backend", selection: $vm.settings.defaultD3DBackend) {
                    Text("None").tag("none")
                    Text("Mock").tag("mock")
                    Text("Metal").tag("metal")
                }
                Toggle("Keep Artifacts Default", isOn: $vm.settings.keepArtifactsDefault)
            }

            Section("Timeouts") {
                TextField("Doctor Timeout", value: $vm.settings.doctorTimeout, format: .number)
                TextField("Integration Timeout", value: $vm.settings.integrationTimeout, format: .number)
            }

            Section("Debug") {
                Toggle("Enable Debug Logs", isOn: $vm.settings.enableDebugLogs)
                Toggle("Enable Mock Mode", isOn: $vm.settings.enableMockMode)
            }

            Section("AI Troubleshoot") {
                // TODO(v0.4): Re-enable Anthropic API-key entry when AI troubleshoot ships as a gated feature.
                // SecureField("Anthropic API Key", text: $anthropicAPIKey)
                Text("AI troubleshoot API is deferred to v0.4.")
                    .foregroundStyle(.secondary)
                TextField("Anthropic Model", text: binding(\.anthropicModelOverride, default: "claude-opus-4-7"))
                Toggle("Share sanitized fixes to community DB", isOn: boolBinding(\.communityFixSharingOptIn))
                HStack {
                    Button("Save Anthropic Key") { saveAnthropicKey() }
                    Text(keychainStatus).font(.caption).foregroundStyle(.secondary)
                }
            }

            Section("Stores") {
                SecureField("Steam API Key", text: $steamAPIKey)
                TextField("SteamID64", text: binding(\.steamID64, default: ""))
                Button("Save Steam Key") { saveSteamKey() }
            }

            Section("Updates & HUD") {
                Picker("Update Channel", selection: binding(\.updateChannel, default: "stable")) {
                    Text("Stable").tag("stable")
                    Text("Beta").tag("beta")
                    Text("Nightly").tag("nightly")
                }
                TextField("HUD Hotkey", text: binding(\.hudHotkey, default: "Cmd+Shift+M"))
                Picker("Language", selection: binding(\.languageOverride, default: "system")) {
                    Text("System").tag("system")
                    Text("English").tag("en")
                    Text("Russian").tag("ru")
                }
                Toggle("Share anonymous telemetry", isOn: boolBinding(\.telemetryOptIn))
            }

            Section("License") {
                let state = LicenseStore().currentState()
                Label(state.message + (state.isTrial ? " · \(state.daysRemaining) days remaining" : ""), systemImage: state.isLicensed ? "checkmark.seal.fill" : "clock.badge.exclamationmark")
                TextField("License Key", text: $licenseKey)
                HStack {
                    Button("Activate") { activateLicense() }
                    Button("Buy License") { PurchaseService().openLifetimePurchase() }
                    Spacer()
                }
            }

            Section("Backup") {
                HStack {
                    Button("Export Settings") { vm.exportSettings() }
                    Button("Import Settings") { vm.importSettings() }
                    Spacer()
                }
            }

            Section("Danger Zone") {
                Button("Reset to Defaults", role: .destructive) {
                    vm.showResetConfirm = true
                }
            }
        }
        .formStyle(.grouped)
        .padding()
        .frame(minWidth: 480)
        .onChange(of: vm.settings.macRunnerRoot) { vm.validateRoot() }
        .alert("Reset all settings?", isPresented: $vm.showResetConfirm) {
            Button("Cancel", role: .cancel) {}
            Button("Reset", role: .destructive) { vm.resetDefaults() }
        } message: {
            Text("This will reset all settings to their default values.")
        }
    }

    private func binding(_ keyPath: WritableKeyPath<AppSettings, String?>, default defaultValue: String) -> Binding<String> {
        Binding(
            get: { vm.settings[keyPath: keyPath] ?? defaultValue },
            set: { vm.settings[keyPath: keyPath] = $0.isEmpty || $0 == "system" ? nil : $0 }
        )
    }

    private func boolBinding(_ keyPath: WritableKeyPath<AppSettings, Bool?>) -> Binding<Bool> {
        Binding(
            get: { vm.settings[keyPath: keyPath] ?? false },
            set: { vm.settings[keyPath: keyPath] = $0 }
        )
    }

    private func saveAnthropicKey() {
        do {
            try AnthropicKeyStore().save(anthropicAPIKey)
            keychainStatus = "Anthropic key saved in Keychain"
            anthropicAPIKey = ""
        } catch {
            keychainStatus = error.localizedDescription
        }
    }

    private func saveSteamKey() {
        do {
            try SteamKeyStore().saveAPIKey(steamAPIKey)
            keychainStatus = "Steam key saved in Keychain"
            steamAPIKey = ""
        } catch {
            keychainStatus = error.localizedDescription
        }
    }

    private func activateLicense() {
        do {
            _ = try LicenseStore().activate(key: licenseKey)
            keychainStatus = "License activated"
            licenseKey = ""
        } catch {
            keychainStatus = error.localizedDescription
        }
    }
}
