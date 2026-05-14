import Foundation
import Testing
@testable import MacRunnerControlCenter

struct CommandRunnerTests {
    @Test @MainActor func echoTest() async throws {
        let runner = CommandRunner()
        runner.run(command: "/bin/echo", arguments: ["hello"], timeout: 5)
        let deadline = Date().addingTimeInterval(5)
        while runner.isRunning && Date() < deadline {
            try await Task.sleep(nanoseconds: 50_000_000)
        }
        #expect(!runner.isRunning)
        #expect(runner.lastResult?.status == .success)
        #expect(runner.lastResult?.stdout.contains("hello") == true)
    }

    @Test @MainActor func timeoutWorks() async throws {
        let runner = CommandRunner()
        runner.run(command: "/bin/sleep", arguments: ["10"], timeout: 1.0)
        let deadline = Date().addingTimeInterval(8)
        while runner.isRunning && Date() < deadline {
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        #expect(!runner.isRunning)
        #expect(runner.lastResult?.status == .timeout)
    }

    @Test @MainActor func cancellationWorks() async throws {
        let runner = CommandRunner()
        runner.run(command: "/bin/sleep", arguments: ["10"], timeout: 60)
        try await Task.sleep(nanoseconds: 100_000_000)
        runner.cancel()
        let deadline = Date().addingTimeInterval(5)
        while runner.isRunning && Date() < deadline {
            try await Task.sleep(nanoseconds: 50_000_000)
        }
        #expect(!runner.isRunning)
    }

    @Test @MainActor func stderrCapture() async throws {
        let runner = CommandRunner()
        runner.run(command: "/bin/bash", arguments: ["-c", "echo error >&2"], timeout: 5)
        let deadline = Date().addingTimeInterval(5)
        while runner.isRunning && Date() < deadline {
            try await Task.sleep(nanoseconds: 50_000_000)
        }
        #expect(!runner.isRunning)
        #expect(runner.lastResult?.stderr.contains("error") == true)
    }
}
