import SwiftUI

@MainActor
final class TaskQueueViewModel: ObservableObject {
    @Published var queue = TaskQueue.shared
    @Published var selectedTask: MacRunnerTask?

    var settings: AppSettings

    init(settings: AppSettings) {
        self.settings = settings
    }

    func cancel(taskId: UUID) {
        queue.cancel(taskId: taskId)
    }

    func cancelAll() {
        queue.cancelAll()
    }

    func retry(taskId: UUID) {
        queue.retry(taskId: taskId)
    }

    func clearCompleted() {
        queue.clearCompleted()
    }

    func enqueueDoctor() {
        let script = "\(settings.macRunnerRoot)/scripts/macr-doctor.sh"
        queue.enqueue(type: .runDoctor, command: script, arguments: [], workingDirectory: settings.macRunnerRoot, timeout: TimeInterval(settings.doctorTimeout))
    }

    func enqueueVerify() {
        let script = "\(settings.macRunnerRoot)/scripts/native-platform-verify.sh"
        queue.enqueue(type: .runVerify, command: script, arguments: [], workingDirectory: settings.macRunnerRoot, timeout: TimeInterval(settings.integrationTimeout))
    }

    func enqueueCleanup() {
        let script = "\(settings.macRunnerRoot)/scripts/cleanup-wine-runtime.py"
        queue.enqueue(type: .cleanupRuntime, command: "/usr/bin/python3", arguments: [script, "--json"], workingDirectory: settings.macRunnerRoot, timeout: 60)
    }

    func taskStatusColor(_ status: MacRunnerTask.TaskStatus) -> Color {
        switch status {
        case .queued: return .secondary
        case .running: return .blue
        case .success: return .green
        case .failed: return .red
        case .cancelled: return .orange
        case .timeout: return .purple
        }
    }

    func taskStatusIcon(_ status: MacRunnerTask.TaskStatus) -> String {
        switch status {
        case .queued: return "circle"
        case .running: return "arrow.triangle.2.circlepath"
        case .success: return "checkmark.circle.fill"
        case .failed: return "xmark.circle.fill"
        case .cancelled: return "slash.circle.fill"
        case .timeout: return "clock.badge.exclamationmark.fill"
        }
    }
}
