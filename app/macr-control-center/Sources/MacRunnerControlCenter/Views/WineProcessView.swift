import SwiftUI

struct WineProcessView: View {
    @EnvironmentObject var settingsVM: SettingsViewModel
    @StateObject private var vm: WineProcessViewModel

    init() {
        self._vm = StateObject(wrappedValue: WineProcessViewModel(settings: SettingsViewModel().settings))
    }

    var body: some View {
        VStack {
            HStack {
                Button("Refresh") { vm.refresh() }
                Button("Cleanup Runtime") { vm.cleanupRuntime() }
                Button("Assert No Leftovers") { vm.assertNoLeftovers() }
                if vm.runner.isRunning {
                    ProgressView().padding(.leading, 8)
                }
                Spacer()
                Toggle("Auto-refresh", isOn: $vm.autoRefresh)
                    .toggleStyle(.switch)
                    .onChange(of: vm.autoRefresh) { _, new in
                        vm.setAutoRefresh(new)
                    }
                TextField("Search", text: $vm.search)
                    .frame(width: 150)
            }
            .padding()

            if vm.processes.isEmpty {
                ContentUnavailableView("No Wine processes", systemImage: "cpu")
                    .frame(maxHeight: .infinity)
            } else {
                Table(of: WineProcess.self) {
                    TableColumn("PID") { proc in Text("\(proc.pid ?? 0)") }
                    TableColumn("Command") { proc in Text(proc.command ?? "—").lineLimit(1) }
                    TableColumn("Age") { proc in Text(proc.age ?? "—") }
                    TableColumn("Reason") { proc in Text(proc.matchedReason ?? "—") }
                    TableColumn("Status") { proc in Text(proc.status ?? "—") }
                } rows: {
                    ForEach(vm.filtered) { proc in
                        TableRow(proc)
                            .contextMenu {
                                Button("Kill") { vm.killProcess(proc) }
                                Button("Copy PID") {
                                    NSPasteboard.general.clearContents()
                                    NSPasteboard.general.setString("\(proc.pid ?? 0)", forType: .string)
                                }
                            }
                    }
                }
                .padding(.horizontal)
            }

            if let err = vm.error {
                Text(err).foregroundStyle(.red).padding()
            }

            HStack {
                Text("\(vm.filtered.count) process(es)")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Spacer()
            }
            .padding(.horizontal)
        }
        .onAppear {
            vm.settings = settingsVM.settings
            vm.refresh()
        }
        .onDisappear {
            vm.stopAutoRefresh()
        }
    }
}
