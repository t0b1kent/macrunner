import SwiftUI

struct TaskQueueView: View {
    @EnvironmentObject var settingsVM: SettingsViewModel
    @StateObject private var vm: TaskQueueViewModel
    @State private var showHistory = false

    init() {
        self._vm = StateObject(wrappedValue: TaskQueueViewModel(settings: SettingsViewModel().settings))
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Button("Doctor") { vm.enqueueDoctor() }
                Button("Verify") { vm.enqueueVerify() }
                Button("Cleanup") { vm.enqueueCleanup() }
                if vm.queue.isExecuting {
                    ProgressView().padding(.leading, 8)
                }
                Spacer()
                Button("History") { showHistory = true }
                Button("Clear Completed") { vm.clearCompleted() }
                    .disabled(vm.queue.tasks.allSatisfy { $0.status == .queued || $0.status == .running })
                Button("Cancel All") { vm.cancelAll() }
                    .disabled(!vm.queue.isExecuting && vm.queue.tasks.allSatisfy { $0.status != .queued })
            }
            .padding()
            .sheet(isPresented: $showHistory) {
                RunHistoryView()
            }

            if vm.queue.tasks.isEmpty {
                ContentUnavailableView("No Tasks", systemImage: "list.bullet.rectangle")
                    .frame(maxHeight: .infinity)
            } else {
                List(selection: $vm.selectedTask) {
                    Section(header: Text("Tasks (\(vm.queue.tasks.count))")) {
                        ForEach(vm.queue.tasks) { task in
                            TaskRow(task: task, vm: vm)
                        }
                    }
                }
                .listStyle(.inset)
            }
        }
        .frame(minWidth: 600)
        .onAppear {
            vm.settings = settingsVM.settings
        }
    }
}

private struct TaskRow: View {
    let task: MacRunnerTask
    @ObservedObject var vm: TaskQueueViewModel

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: vm.taskStatusIcon(task.status))
                .foregroundStyle(vm.taskStatusColor(task.status))
                .font(.title3)

            VStack(alignment: .leading, spacing: 2) {
                Text(task.type.rawValue)
                    .font(.headline)
                Text(URL(fileURLWithPath: task.command).lastPathComponent)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }

            Spacer()

            if let duration = task.durationMs {
                Text("\(duration)ms")
                    .font(.caption2)
                    .monospacedDigit()
                    .foregroundStyle(.secondary)
            }

            Text(task.status.rawValue)
                .font(.caption)
                .padding(.horizontal, 6)
                .padding(.vertical, 2)
                .background(vm.taskStatusColor(task.status).opacity(0.15))
                .cornerRadius(4)

            HStack(spacing: 4) {
                if task.status == .running {
                    Button {
                        vm.cancel(taskId: task.id)
                    } label: {
                        Image(systemName: "stop.fill")
                    }
                    .buttonStyle(.plain)
                    .foregroundStyle(.red)
                }
                if task.status == .failed || task.status == .timeout || task.status == .cancelled {
                    Button {
                        vm.retry(taskId: task.id)
                    } label: {
                        Image(systemName: "arrow.clockwise")
                    }
                    .buttonStyle(.plain)
                }
            }
        }
        .padding(.vertical, 4)
        .tag(task)
    }
}
