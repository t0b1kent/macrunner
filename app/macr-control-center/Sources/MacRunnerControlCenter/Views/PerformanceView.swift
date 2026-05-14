import SwiftUI

struct PerformanceView: View {
    @EnvironmentObject var settingsVM: SettingsViewModel
    @StateObject private var vm: PerformanceViewModel

    init() {
        self._vm = StateObject(wrappedValue: PerformanceViewModel(settings: SettingsViewModel().settings))
    }

    var body: some View {
        VStack {
            HStack {
                Button("Refresh") { vm.refresh() }
                Button("Export CSV") { vm.exportCSV() }
                Button("Export JSON") { vm.exportJSON() }
                Spacer()
                Picker("Range", selection: $vm.timeRange) {
                    ForEach(TimeRange.allCases, id: \.self) { range in
                        Text(range.rawValue).tag(range)
                    }
                }
                .pickerStyle(.segmented)
                .frame(width: 200)
                if !vm.backends.isEmpty {
                    Picker("Backend", selection: $vm.selectedBackend) {
                        Text("All").tag(nil as String?)
                        ForEach(vm.backends, id: \.self) { b in
                            Text(b).tag(b as String?)
                        }
                    }
                    .frame(width: 120)
                }
            }
            .padding()

            if vm.filteredHistory.isEmpty {
                ContentUnavailableView("No performance data", systemImage: "chart.bar")
                    .frame(maxHeight: .infinity)
            } else {
                ScrollView {
                    VStack(alignment: .leading, spacing: 16) {
                        // Stats cards
                        HStack(spacing: 12) {
                            StatCard(title: "Runs", value: "\(vm.stats.totalRuns)", color: .blue)
                            StatCard(title: "Success", value: "\(Int(vm.stats.successRate * 100))%", color: .green)
                            StatCard(title: "Avg", value: "\(vm.stats.avgMs)ms", color: .secondary)
                            if vm.stats.crashes > 0 {
                                StatCard(title: "Crashes", value: "\(vm.stats.crashes)", color: .red)
                            }
                            if vm.stats.timeouts > 0 {
                                StatCard(title: "Timeouts", value: "\(vm.stats.timeouts)", color: .orange)
                            }
                        }

                        // Backend breakdown
                        if !vm.backendBreakdown.isEmpty {
                            Text("Backend Breakdown").font(.headline)
                            VStack(spacing: 6) {
                                ForEach(vm.backendBreakdown, id: \.0) { backend, passes, fails, avg in
                                    HStack {
                                        Text(backend).font(.caption)
                                            .frame(width: 80, alignment: .leading)
                                        HStack(spacing: 0) {
                                            Rectangle()
                                                .fill(Color.green)
                                                .frame(width: CGFloat(passes) * 4, height: 8)
                                            Rectangle()
                                                .fill(Color.red)
                                                .frame(width: CGFloat(fails) * 4, height: 8)
                                        }
                                        .cornerRadius(4)
                                        Text("\(passes) pass / \(fails) fail")
                                            .font(.caption2)
                                            .foregroundStyle(.secondary)
                                        Spacer()
                                        Text("avg \(avg)ms")
                                            .font(.caption2)
                                            .monospacedDigit()
                                            .foregroundStyle(.secondary)
                                    }
                                }
                            }
                        }

                        // Duration histogram
                        if !vm.histogram.isEmpty {
                            Text("Duration Distribution").font(.headline)
                            VStack(spacing: 4) {
                                ForEach(vm.histogram, id: \.0) { threshold, count in
                                    HStack {
                                        Text(threshold == 30000 ? "30s+" : "\(threshold)ms")
                                            .font(.caption2)
                                            .frame(width: 50, alignment: .trailing)
                                            .foregroundStyle(.secondary)
                                        GeometryReader { geo in
                                            Rectangle()
                                                .fill(Color.accentColor)
                                                .frame(width: geo.size.width * barWidth(count: count, max: vm.histogram.map { $0.1 }.max() ?? 1))
                                                .cornerRadius(2)
                                        }
                                        .frame(height: 12)
                                        Text("\(count)")
                                            .font(.caption2)
                                            .monospacedDigit()
                                            .foregroundStyle(.secondary)
                                            .frame(width: 30, alignment: .leading)
                                    }
                                }
                            }
                        }

                        // Recent runs
                        Text("Recent Runs (last 30)").font(.headline)
                        VStack(spacing: 4) {
                            ForEach(vm.filteredHistory.prefix(30)) { h in
                                HStack {
                                    Text(h.status).font(.caption.bold())
                                        .foregroundStyle(statusColor(h.status))
                                        .frame(width: 60, alignment: .leading)
                                    Text("\(h.durationMs) ms").font(.caption).monospacedDigit()
                                        .frame(width: 70, alignment: .trailing)
                                    Text(h.d3dBackend ?? "—").font(.caption2).foregroundStyle(.secondary)
                                        .frame(width: 60, alignment: .leading)
                                    Spacer()
                                    Text(Formatters.mediumDate.string(from: h.timestamp))
                                        .font(.caption2)
                                        .foregroundStyle(.secondary)
                                }
                            }
                        }
                    }
                    .padding()
                }
            }
        }
        .onAppear {
            vm.settings = settingsVM.settings
            vm.refresh()
        }
    }

    private func barWidth(count: Int, max: Int) -> CGFloat {
        guard max > 0 else { return 0 }
        return CGFloat(count) / CGFloat(max)
    }

    private func statusColor(_ status: String) -> Color {
        switch status {
        case "PASS": return .green
        case "FAIL": return .red
        case "CRASH": return .orange
        case "TIMEOUT": return .yellow
        default: return .gray
        }
    }
}

private struct StatCard: View {
    let title: String
    let value: String
    let color: Color

    var body: some View {
        VStack(spacing: 4) {
            Text(title).font(.caption2).foregroundStyle(.secondary)
            Text(value).font(.title3.bold()).foregroundStyle(color)
        }
        .frame(minWidth: 60)
        .padding(8)
        .background(Color(.controlBackgroundColor))
        .cornerRadius(8)
    }
}
