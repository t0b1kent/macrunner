import SwiftUI

struct MinimalSettingsSheet: View {
    @Environment(\.colorScheme) private var scheme
    @EnvironmentObject var settingsVM: SettingsViewModel
    @Binding var developerMode: Bool
    let onClose: () -> Void

    @State private var rootPath: String = ""
    @State private var bottlesPath: String = ""
    @State private var timeoutText: String = ""
    @State private var keepArtifacts: Bool = false
    @State private var debugLogs: Bool = false

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider().background(Theme.Palette.separator(scheme))
            ScrollView {
                VStack(alignment: .leading, spacing: Theme.Spacing.xl) {
                    section(title: "Engine") {
                        pathRow(label: "MacRunner root", value: $rootPath) {
                            chooseFolder { url in
                                rootPath = url.path
                                settingsVM.settings.macRunnerRoot = url.path
                                ConfigStore.shared.saveSettings(settingsVM.settings)
                            }
                        }
                        pathRow(label: "Bottles directory", value: $bottlesPath) {
                            chooseFolder { url in
                                bottlesPath = url.path
                                settingsVM.settings.bottlesDirectory = url.path
                                ConfigStore.shared.saveSettings(settingsVM.settings)
                            }
                        }
                    }

                    section(title: "Defaults") {
                        fieldRow(label: "Default timeout") {
                            HStack(spacing: 6) {
                                TextField("", text: $timeoutText)
                                    .textFieldStyle(.plain)
                                    .frame(width: 60, alignment: .trailing)
                                    .padding(.vertical, 6)
                                    .padding(.horizontal, 10)
                                    .overlay(
                                        RoundedRectangle(cornerRadius: Theme.Radius.small)
                                            .stroke(Theme.Palette.border(scheme), lineWidth: 0.5)
                                    )
                                    .onSubmit { commitTimeout() }
                                Text("seconds")
                                    .font(Theme.Font.caption)
                                    .foregroundColor(Theme.Palette.textTertiary(scheme))
                            }
                        }
                        toggleRow(label: "Keep run artifacts", isOn: $keepArtifacts) {
                            settingsVM.settings.keepArtifactsDefault = keepArtifacts
                            ConfigStore.shared.saveSettings(settingsVM.settings)
                        }
                        toggleRow(label: "Verbose debug logs", isOn: $debugLogs) {
                            settingsVM.settings.enableDebugLogs = debugLogs
                            ConfigStore.shared.saveSettings(settingsVM.settings)
                        }
                    }

                    section(title: "Advanced") {
                        VStack(alignment: .leading, spacing: Theme.Spacing.m) {
                            toggleRow(label: "Developer mode", isOn: $developerMode) {}
                            Text("Unlocks engine internals: D3D artifacts, perf, doctor, corpus, compatibility DB, debug bundles. For developers and power users.")
                                .font(Theme.Font.caption)
                                .foregroundColor(Theme.Palette.textTertiary(scheme))
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                }
                .padding(Theme.Spacing.xl)
            }
            Divider().background(Theme.Palette.separator(scheme))
            footer
        }
        .frame(width: 560, height: 620)
        .background(Theme.Palette.bgPrimary(scheme))
        .onAppear { loadFromSettings() }
    }

    private var header: some View {
        HStack {
            Text("Settings")
                .font(Theme.Font.title)
                .foregroundColor(Theme.Palette.textPrimary(scheme))
            Spacer()
            Button(action: onClose) {
                Image(systemName: "xmark")
                    .font(.system(size: 11, weight: .medium))
                    .foregroundColor(Theme.Palette.textSecondary(scheme))
                    .frame(width: 28, height: 28)
                    .background(
                        Circle()
                            .stroke(Theme.Palette.border(scheme), lineWidth: 0.5)
                    )
            }
            .buttonStyle(.scalePress)
            .keyboardShortcut(.escape, modifiers: [])
        }
        .padding(Theme.Spacing.xl)
    }

    private var footer: some View {
        HStack {
            Text("v\(appVersion)")
                .font(Theme.Font.monoCaption)
                .foregroundColor(Theme.Palette.textTertiary(scheme))
            Spacer()
            Button("Done", action: onClose)
                .buttonStyle(.minimalPrimary)
                .keyboardShortcut(.return, modifiers: [])
        }
        .padding(Theme.Spacing.l)
    }

    private func section<Content: View>(
        title: String,
        @ViewBuilder content: () -> Content
    ) -> some View {
        VStack(alignment: .leading, spacing: Theme.Spacing.m) {
            Text(title.uppercased())
                .font(Theme.Font.monoCaption)
                .foregroundColor(Theme.Palette.textTertiary(scheme))
                .kerning(1)
            VStack(alignment: .leading, spacing: Theme.Spacing.m) {
                content()
            }
        }
    }

    private func pathRow(label: String, value: Binding<String>, onChoose: @escaping () -> Void) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(label)
                .font(Theme.Font.body)
                .foregroundColor(Theme.Palette.textPrimary(scheme))
            HStack(spacing: 6) {
                Text(value.wrappedValue.isEmpty ? "Not set" : value.wrappedValue)
                    .font(Theme.Font.mono)
                    .foregroundColor(value.wrappedValue.isEmpty ? Theme.Palette.textTertiary(scheme) : Theme.Palette.textSecondary(scheme))
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .frame(maxWidth: .infinity, alignment: .leading)
                Button("Choose…", action: onChoose)
                    .buttonStyle(.minimalGhost)
            }
            .padding(.vertical, 6)
            .padding(.horizontal, 10)
            .background(
                RoundedRectangle(cornerRadius: Theme.Radius.small)
                    .fill(Theme.Palette.bgSecondary(scheme))
            )
        }
    }

    private func fieldRow<Content: View>(label: String, @ViewBuilder content: () -> Content) -> some View {
        HStack {
            Text(label)
                .font(Theme.Font.body)
                .foregroundColor(Theme.Palette.textPrimary(scheme))
            Spacer()
            content()
        }
    }

    private func toggleRow(label: String, isOn: Binding<Bool>, onChange: @escaping () -> Void) -> some View {
        HStack {
            Text(label)
                .font(Theme.Font.body)
                .foregroundColor(Theme.Palette.textPrimary(scheme))
            Spacer()
            Toggle("", isOn: isOn)
                .labelsHidden()
                .toggleStyle(.switch)
                .tint(Theme.Palette.emphasis(scheme))
                .onChange(of: isOn.wrappedValue) { _, _ in onChange() }
        }
    }

    private func loadFromSettings() {
        rootPath = settingsVM.settings.macRunnerRoot
        bottlesPath = settingsVM.settings.bottlesDirectory
        timeoutText = "\(settingsVM.settings.defaultTimeout)"
        keepArtifacts = settingsVM.settings.keepArtifactsDefault
        debugLogs = settingsVM.settings.enableDebugLogs
    }

    private func commitTimeout() {
        guard let value = Int(timeoutText), value > 0 else {
            timeoutText = "\(settingsVM.settings.defaultTimeout)"
            return
        }
        settingsVM.settings.defaultTimeout = value
        ConfigStore.shared.saveSettings(settingsVM.settings)
    }

    private var appVersion: String {
        Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "dev"
    }

    private func chooseFolder(_ onPick: @escaping (URL) -> Void) {
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        if panel.runModal() == .OK, let url = panel.url {
            onPick(url)
        }
    }
}
