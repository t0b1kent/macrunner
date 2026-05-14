import Foundation

@MainActor
final class TaskQueue: ObservableObject {
    static let shared = TaskQueue()

    @Published var tasks: [MacRunnerTask] = []
    @Published var isExecuting = false

    private var runner = CommandRunner()
    private var executionTask: Task<Void, Never>?

    func enqueue(_ task: MacRunnerTask) {
        tasks.append(task)
        if !isExecuting {
            processNext()
        }
    }

    func enqueue(type: MacRunnerTask.TaskType, command: String, arguments: [String] = [], workingDirectory: String? = nil, environment: [String: String]? = nil, timeout: TimeInterval = 300) {
        let task = MacRunnerTask(type: type, command: command, arguments: arguments, workingDirectory: workingDirectory, environment: environment, timeout: timeout)
        enqueue(task)
    }

    func cancel(taskId: UUID) {
        if let index = tasks.firstIndex(where: { $0.id == taskId }) {
            if tasks[index].status == .running {
                runner.cancel()
                executionTask?.cancel()
            }
            tasks[index].status = .cancelled
            tasks[index].completedAt = Date()
        }
    }

    func cancelAll() {
        runner.cancel()
        executionTask?.cancel()
        for i in tasks.indices where tasks[i].status == .queued || tasks[i].status == .running {
            tasks[i].status = .cancelled
            tasks[i].completedAt = Date()
        }
        isExecuting = false
    }

    func retry(taskId: UUID) {
        if let index = tasks.firstIndex(where: { $0.id == taskId }) {
            tasks[index].status = .queued
            tasks[index].startedAt = nil
            tasks[index].completedAt = nil
            tasks[index].exitCode = nil
            tasks[index].durationMs = nil
            tasks[index].errorMessage = nil
            tasks[index].logs = []
            if !isExecuting {
                processNext()
            }
        }
    }

    func clearCompleted() {
        tasks.removeAll { $0.status != .queued && $0.status != .running }
    }

    private func processNext() {
        guard let index = tasks.firstIndex(where: { $0.status == .queued }) else {
            isExecuting = false
            return
        }
        isExecuting = true
        tasks[index].status = .running
        tasks[index].startedAt = Date()
        let task = tasks[index]

        executionTask = Task {
            runner.run(
                command: task.command,
                arguments: task.arguments,
                workingDirectory: task.workingDirectory,
                environment: task.environment,
                timeout: task.timeout
            )

            while runner.isRunning {
                try? await Task.sleep(nanoseconds: 200_000_000)
                if Task.isCancelled {
                    runner.cancel()
                    return
                }
            }

            await MainActor.run {
                if let result = self.runner.lastResult {
                    self.updateTask(id: task.id, result: result)
                }
                if !Task.isCancelled {
                    self.processNext()
                }
            }
        }
    }

    private func updateTask(id: UUID, result: CommandResult) {
        if let index = tasks.firstIndex(where: { $0.id == id }) {
            switch result.status {
            case .success:
                tasks[index].status = .success
            case .timeout:
                tasks[index].status = .timeout
            case .cancelled:
                tasks[index].status = .cancelled
            default:
                tasks[index].status = .failed
            }
            tasks[index].exitCode = result.exitCode
            tasks[index].durationMs = result.durationMs
            var logs = tasks[index].logs
            if !result.stdout.isEmpty { logs.append(result.stdout) }
            if !result.stderr.isEmpty { logs.append(result.stderr) }
            tasks[index].logs = logs
            tasks[index].completedAt = Date()
            if result.status != .success {
                tasks[index].errorMessage = result.stderr.isEmpty ? "Exit code \(result.exitCode)" : result.stderr
            }
            RunHistoryStore.shared.append(tasks[index])
        }
    }
}
