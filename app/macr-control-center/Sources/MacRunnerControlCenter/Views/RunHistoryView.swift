import SwiftUI

struct RunHistoryView: View {
    @StateObject private var vm = RunHistoryViewModel()
    @State private var showConfirmClear = false

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text("Run History")
                    .font(.headline)
                Spacer()
                Button("Refresh") { vm.refresh() }
                Button("Clear All") { showConfirmClear = true }
                    .disabled(vm.entries.isEmpty)
            }
            .padding()

            if vm.entries.isEmpty {
                ContentUnavailableView("No History", systemImage: "clock.arrow.circlepath")
                    .frame(maxHeight: .infinity)
            } else {
                List(selection: $vm.selectedTask) {
                    Section(header: Text("\(vm.entries.count) entries (last 200 kept)")) {
                        ForEach(vm.entries) { entry in
                            HistoryRow(entry: entry, vm: vm)
                        }
                    }
                }
                .listStyle(.inset)
            }
        }
        .frame(minWidth: 600)
        .alert("Clear all history?", isPresented: $showConfirmClear) {
            Button("Cancel", role: .cancel) {}
            Button("Clear", role: .destructive) { vm.clearAll() }
        } message: {
            Text("This cannot be undone.")
        }
    }
}

private struct HistoryRow: View {
    let entry: MacRunnerTask
    @ObservedObject var vm: RunHistoryViewModel

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: vm.taskStatusIcon(entry.status))
                .foregroundStyle(vm.taskStatusColor(entry.status))
                .font(.title3)

            VStack(alignment: .leading, spacing: 2) {
                Text(entry.type.rawValue)
                    .font(.headline)
                Text(URL(fileURLWithPath: entry.command).lastPathComponent)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                if let completed = entry.completedAt {
                    Text(completed, style: .date)
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
            }

            Spacer()

            if let duration = entry.durationMs {
                Text("\(duration)ms")
                    .font(.caption2)
                    .monospacedDigit()
                    .foregroundStyle(.secondary)
            }

            Text(entry.status.rawValue)
                .font(.caption)
                .padding(.horizontal, 6)
                .padding(.vertical, 2)
                .background(vm.taskStatusColor(entry.status).opacity(0.15))
                .cornerRadius(4)

            Button {
                vm.remove(taskId: entry.id)
            } label: {
                Image(systemName: "trash")
            }
            .buttonStyle(.plain)
            .foregroundStyle(.secondary)
        }
        .padding(.vertical, 4)
        .tag(entry)
    }
}
