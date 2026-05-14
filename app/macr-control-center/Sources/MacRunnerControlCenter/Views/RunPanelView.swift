import SwiftUI

struct RunPanelView: View {
    @EnvironmentObject var settingsVM: SettingsViewModel
    @StateObject private var vm: RunAppViewModel
    @State private var exePath: String = ""
    @State private var args: String = ""
    @State private var workdir: String = ""
    @State private var envString: String = ""
    @State private var timeout: String = "45"
    @State private var d3dBackend: String = "none"
    @State private var keepArtifacts: Bool = false
    @State private var d3dTrace: Bool = false
    @State private var debugMode: Bool = false
    @State private var showPicker = false

    init() {
        self._vm = StateObject(wrappedValue: RunAppViewModel(settings: SettingsViewModel().settings))
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Form {
                Section("Executable") {
                    HStack {
                        TextField("Path to .exe", text: $exePath)
                        Button("Choose…") {
                            let panel = NSOpenPanel()
                            panel.allowsMultipleSelection = false
                            panel.canChooseDirectories = false
                            if panel.runModal() == .OK {
                                exePath = panel.url?.path ?? ""
                            }
                        }
                    }
                    TextField("Arguments", text: $args)
                    TextField("Working Directory", text: $workdir)
                    TextField("Environment (KEY=VAL, one per line)", text: $envString, axis: .vertical)
                        .lineLimit(2...6)
                }
                Section("Options") {
                    HStack {
                        TextField("Timeout (s)", text: $timeout)
                            .frame(width: 80)
                        Picker("D3D Backend", selection: $d3dBackend) {
                            Text("None").tag("none")
                            Text("Mock").tag("mock")
                            Text("Metal").tag("metal")
                        }
                        .frame(width: 160)
                    }
                    Toggle("Keep Artifacts", isOn: $keepArtifacts)
                    Toggle("D3D Trace", isOn: $d3dTrace)
                    Toggle("Debug Mode", isOn: $debugMode)
                }
                Section("Actions") {
                    HStack {
                        Button("Run") { run() }
                            .disabled(vm.runner.isRunning || exePath.isEmpty)
                        Button("Cancel") { vm.cancel() }
                            .disabled(!vm.runner.isRunning)
                        if vm.runner.isRunning {
                            ProgressView().padding(.leading, 8)
                        }
                        Spacer()
                        StatusBadge(status: vm.lastLauncherResult?.status)
                    }
                }

                if let result = vm.lastLauncherResult, result.status != "PASS" {
                    FailureTriageView(result: result, exePath: exePath)
                }
            }
            .formStyle(.grouped)

            if let result = vm.lastLauncherResult, result.d3dEnabled == true || result.d3dStatus != nil {
                InlineD3DArtifactsView(result: result)
                    .padding(.horizontal)
                Divider()
            }

            HStack(spacing: 0) {
                VStack(alignment: .leading) {
                    Text("Stdout").font(.headline).padding(.horizontal)
                    LogTextView(text: vm.runner.stdoutBuffer)
                        .padding(.horizontal)
                }
                Divider()
                VStack(alignment: .leading) {
                    Text("Stderr").font(.headline).padding(.horizontal)
                    LogTextView(text: vm.runner.stderrBuffer)
                        .padding(.horizontal)
                        .foregroundStyle(.red)
                }
            }
            .frame(maxHeight: .infinity)
        }
        .onAppear {
            keepArtifacts = settingsVM.settings.keepArtifactsDefault
            debugMode = settingsVM.settings.enableDebugLogs
            vm.settings = settingsVM.settings
        }
    }

    private func run() {
        let app = AppEntry.new(name: (exePath as NSString).lastPathComponent, exePath: exePath)
        var entry = app
        entry.args = args.split(separator: " ").map(String.init)
        entry.workdir = workdir.isEmpty ? nil : workdir
        entry.timeout = Int(timeout) ?? 45
        entry.d3dBackend = d3dBackend
        var env: [String: String] = [:]
        for line in envString.split(separator: "\n") {
            let parts = line.split(separator: "=", maxSplits: 1)
            if parts.count == 2 {
                env[String(parts[0])] = String(parts[1])
            }
        }
        entry.env = env.isEmpty ? nil : env
        vm.settings = settingsVM.settings
        vm.run(app: entry)
    }
}
