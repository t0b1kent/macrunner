import Foundation
import Testing
@testable import MacRunnerControlCenter

struct MacRunnerTaskTests {
    @Test func taskDefaults() {
        let task = MacRunnerTask(type: .runDoctor, command: "/bin/echo")
        #expect(task.status == .queued)
        #expect(task.arguments.isEmpty)
        #expect(task.workingDirectory == nil)
        #expect(task.timeout == 300)
        #expect(task.logs.isEmpty)
        #expect(task.artifactPaths.isEmpty)
    }

    @Test func taskStatuses() {
        let statuses: [MacRunnerTask.TaskStatus] = [.queued, .running, .success, .failed, .cancelled, .timeout]
        #expect(statuses.count == 6)
    }

    @Test func taskTypes() {
        let types: [MacRunnerTask.TaskType] = [.runApp, .runDoctor, .runVerify, .runD3DSmoke, .runWinAPIMatrix, .runRealAppCorpus, .cleanupRuntime, .exportDebugBundle, .packageApp, .customCommand]
        #expect(types.count == 10)
    }

    @Test func taskCodable() throws {
        let task = MacRunnerTask(type: .runVerify, command: "/bin/ls", arguments: ["-la"], workingDirectory: "/tmp", timeout: 60)
        let data = try JSONEncoder().encode(task)
        let decoded = try JSONDecoder().decode(MacRunnerTask.self, from: data)
        #expect(decoded.type == .runVerify)
        #expect(decoded.command == "/bin/ls")
        #expect(decoded.arguments == ["-la"])
        #expect(decoded.workingDirectory == "/tmp")
        #expect(decoded.timeout == 60)
    }
}
