import SwiftUI

@MainActor
final class RunHistoryViewModel: ObservableObject {
    @Published var entries: [MacRunnerTask] = []
    @Published var selectedTask: MacRunnerTask?

    init() {
        refresh()
    }

    func refresh() {
        entries = RunHistoryStore.shared.load()
    }

    func remove(taskId: UUID) {
        RunHistoryStore.shared.remove(taskId: taskId)
        refresh()
    }

    func clearAll() {
        RunHistoryStore.shared.clear()
        refresh()
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
