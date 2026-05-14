import Foundation
import Testing
@testable import MacRunnerControlCenter

struct TaskQueueTests {
    @Test @MainActor func enqueueAndStatus() {
        let queue = TaskQueue()
        #expect(queue.tasks.isEmpty)
        #expect(!queue.isExecuting)

        let task = MacRunnerTask(type: .runDoctor, command: "/bin/echo", arguments: ["hello"])
        queue.enqueue(task)

        #expect(queue.tasks.count == 1)
        // Enqueue immediately starts execution, so status becomes running
        #expect(queue.tasks[0].status == .running)
    }

    @Test @MainActor func cancelAll() {
        let queue = TaskQueue()
        let task = MacRunnerTask(type: .runVerify, command: "/bin/echo", arguments: ["test"])
        queue.enqueue(task)
        queue.cancelAll()

        #expect(queue.tasks[0].status == .cancelled)
        #expect(!queue.isExecuting)
    }

    @Test @MainActor func clearCompleted() {
        let queue = TaskQueue()
        let task = MacRunnerTask(type: .cleanupRuntime, command: "/bin/echo", arguments: ["done"])
        queue.enqueue(task)
        queue.cancelAll()
        queue.clearCompleted()

        #expect(queue.tasks.isEmpty)
    }

    @Test @MainActor func retry() {
        let queue = TaskQueue()
        let task = MacRunnerTask(type: .runDoctor, command: "/bin/echo", arguments: ["retry"])
        queue.enqueue(task)
        queue.cancelAll()
        queue.retry(taskId: task.id)

        // Retry immediately starts execution
        #expect(queue.tasks[0].status == .running)
        #expect(queue.tasks[0].completedAt == nil)
    }
}
