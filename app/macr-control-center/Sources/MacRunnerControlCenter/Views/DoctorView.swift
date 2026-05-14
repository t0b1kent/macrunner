import SwiftUI

struct DoctorView: View {
    @EnvironmentObject var settingsVM: SettingsViewModel
    @StateObject private var vm: DoctorViewModel
    @State private var rawTab = 0
    @State private var showRemediation = false
    @State private var remediationCommand: String?

    init() {
        self._vm = StateObject(wrappedValue: DoctorViewModel(settings: SettingsViewModel().settings))
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                Button("Run Doctor") { vm.runDoctor() }
                Button("Quick Verify") { vm.runQuickVerify() }
                Button("Check Leftovers") { vm.runCheckLeftovers() }
                Button("Cleanup") { vm.runCleanup() }
                Button("Export") { vm.exportReport() }
                    .disabled(vm.doctorReport == nil && vm.verifyReport == nil)
                if vm.runner.isRunning {
                    ProgressView().padding(.leading, 8)
                    Button("Cancel") { vm.runner.cancel() }
                }
                Spacer()
                if vm.doctorReport != nil {
                    HealthBadge(score: vm.healthScore)
                }
            }
            .padding()

            if let report = vm.doctorReport {
                doctorDetail(report: report)
            }

            if let verify = vm.verifyReport {
                verifyDetail(verify: verify)
            }

            if !vm.history.isEmpty {
                HStack {
                    Text("Recent runs:").font(.caption).foregroundStyle(.secondary)
                    ForEach(vm.history.prefix(5), id: \.self) { item in
                        Text(item)
                            .font(.caption2)
                            .padding(.horizontal, 4)
                            .padding(.vertical, 2)
                            .background(Color.secondary.opacity(0.1))
                            .cornerRadius(4)
                    }
                    Spacer()
                }
                .padding(.horizontal)
            }

            if vm.recommendations.isEmpty {
                EmptyView()
            } else {
                VStack(alignment: .leading, spacing: 8) {
                    Text("Recommendations")
                        .font(.headline)
                    ForEach(vm.recommendations, id: \.self) { rec in
                        HStack {
                            Image(systemName: "lightbulb.fill")
                                .foregroundStyle(.yellow)
                            Text(rec)
                                .font(.caption)
                            Spacer()
                        }
                        .padding(.horizontal, 8)
                        .padding(.vertical, 6)
                        .background(Color.yellow.opacity(0.1))
                        .cornerRadius(6)
                    }
                }
                .padding(.horizontal)
            }

            Picker("Output", selection: $rawTab) {
                Text("Stdout").tag(0)
                Text("Stderr").tag(1)
            }
            .pickerStyle(.segmented)
            .padding(.horizontal)

            TabView(selection: $rawTab) {
                LogTextView(text: vm.runner.stdoutBuffer)
                    .tag(0)
                LogTextView(text: vm.runner.stderrBuffer)
                    .tag(1)
            }
            .padding(.horizontal)
            .frame(maxHeight: .infinity)
        }
        .onAppear {
            vm.settings = settingsVM.settings
        }
    }

    @ViewBuilder
    private func doctorDetail(report: DoctorReport) -> some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                if let host = report.host {
                    HStack {
                        Text(host.system ?? "?")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        Text(host.macos ?? "?")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        Spacer()
                    }
                }

                // Health cards
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 180))], spacing: 12) {
                    DoctorCard(title: "ARM64 Lane", pass: report.lanes?.arm64?.exists == true)
                    DoctorCard(title: "x64 Lane", pass: report.lanes?.x64?.exists == true)
                    DoctorCard(title: "x86 Lane", pass: report.lanes?.x86?.exists == true)
                    DoctorCard(title: "Metal", pass: report.graphics?.metalProbePass == true)
                    DoctorCard(title: "D3D Bridge", pass: report.graphics?.renderCore == true)
                    DoctorCard(title: "Scripts", pass: report.scripts?.runWindowsApp?.exists == true)
                    if let d3d = report.scripts?.d3dSmoke?.exists {
                        DoctorCard(title: "D3D Smoke", pass: d3d)
                    }
                }
                .padding(.horizontal)

                // Tools detail
                if let tools = report.tools {
                    detailSection(title: "Tools") {
                        HStack {
                            toolRow(name: "Clang", present: tools.clang == true)
                            if let py = tools.python {
                                toolRow(name: "Python", present: true)
                                Text(py).font(.caption2).foregroundStyle(.secondary)
                            } else {
                                toolRow(name: "Python", present: false)
                            }
                            Spacer()
                        }
                    }
                }

                // Lanes detail
                if let lanes = report.lanes {
                    detailSection(title: "Lanes") {
                        VStack(alignment: .leading, spacing: 6) {
                            laneRow(name: "ARM64", status: lanes.arm64)
                            laneRow(name: "x64", status: lanes.x64)
                            laneRow(name: "x86", status: lanes.x86)
                        }
                    }
                }

                // Graphics detail
                if let graphics = report.graphics {
                    detailSection(title: "Graphics") {
                        VStack(alignment: .leading, spacing: 6) {
                            HStack {
                                Text("Render Core")
                                Spacer()
                                StatusDot(pass: graphics.renderCore == true)
                            }
                            HStack {
                                Text("Metal Probe")
                                Spacer()
                                StatusDot(pass: graphics.metalProbePass == true)
                            }
                            if let stdout = graphics.metalProbeStdout, !stdout.isEmpty {
                                Text("Metal Probe Output")
                                    .font(.caption.bold())
                                Text(stdout)
                                    .font(.system(.caption, design: .monospaced))
                                    .textSelection(.enabled)
                                    .padding(8)
                                    .background(Color(.textBackgroundColor))
                                    .cornerRadius(6)
                            }
                        }
                    }
                }

                // Scripts detail
                if let scripts = report.scripts {
                    detailSection(title: "Scripts") {
                        VStack(alignment: .leading, spacing: 6) {
                            laneRow(name: "run-windows-app.sh", status: scripts.runWindowsApp)
                            laneRow(name: "d3d-smoke", status: scripts.d3dSmoke)
                        }
                    }
                }
            }
            .padding(.vertical)
        }
        .frame(maxHeight: 280)
    }

    @ViewBuilder
    private func verifyDetail(verify: PlatformVerifyReport) -> some View {
        HStack {
            Text("Verify:").font(.headline)
            StatusBadge(status: verify.status)
            Spacer()
        }
        .padding(.horizontal)
    }

    private func detailSection(title: String, @ViewBuilder content: () -> some View) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title).font(.headline)
            content()
        }
        .padding()
        .background(Color(.controlBackgroundColor))
        .cornerRadius(8)
        .padding(.horizontal)
    }

    private func toolRow(name: String, present: Bool) -> some View {
        HStack(spacing: 6) {
            StatusDot(pass: present)
            Text(name).font(.caption)
        }
    }

    private func laneRow(name: String, status: LaneStatus?) -> some View {
        HStack {
            Text(name).font(.caption)
            Spacer()
            if let path = status?.path {
                Text(path).font(.caption2).foregroundStyle(.secondary).lineLimit(1)
            }
            if let exists = status?.exists {
                Image(systemName: exists ? "checkmark.circle.fill" : "xmark.circle.fill")
                    .foregroundStyle(exists ? .green : .red)
                    .font(.caption)
            }
            if status?.executable == false {
                Text("not executable").font(.caption2).foregroundStyle(.orange)
            }
        }
    }
}

struct DoctorCard: View {
    let title: String
    let pass: Bool
    var body: some View {
        HStack {
            Image(systemName: pass ? "checkmark.seal.fill" : "xmark.octagon.fill")
                .foregroundStyle(pass ? .green : .red)
            Text(title).font(.caption.bold())
            Spacer()
        }
        .padding(10)
        .background(Color(.controlBackgroundColor))
        .cornerRadius(8)
    }
}

struct HealthBadge: View {
    let score: Int
    var body: some View {
        HStack(spacing: 4) {
            Image(systemName: score >= 80 ? "heart.fill" : score >= 50 ? "exclamationmark.triangle.fill" : "xmark.shield.fill")
                .foregroundStyle(score >= 80 ? .green : score >= 50 ? .orange : .red)
            Text("\(score)%")
                .font(.caption.bold())
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 4)
        .background(Color(.controlBackgroundColor))
        .cornerRadius(8)
    }
}

struct StatusDot: View {
    let pass: Bool
    var body: some View {
        Circle()
            .fill(pass ? Color.green : Color.red)
            .frame(width: 8, height: 8)
    }
}
