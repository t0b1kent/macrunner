import Foundation
import Testing
@testable import MacRunnerControlCenter

struct LogStreamerTests {
    @Test @MainActor func echoStreaming() async throws {
        let streamer = LogStreamer()
        streamer.run(command: "/bin/echo", arguments: ["hello", "world"], timeout: 5)
        let deadline = Date().addingTimeInterval(5)
        while streamer.isRunning && Date() < deadline {
            try await Task.sleep(nanoseconds: 50_000_000)
        }
        #expect(!streamer.isRunning)
        let texts = streamer.lines.map { $0.text }
        #expect(texts.contains { $0.contains("hello") })
        #expect(texts.contains { $0.contains("world") })
    }

    @Test func classifyLines() {
        #expect(LogStreamer.classify(line: "PASS") == .pass)
        #expect(LogStreamer.classify(line: "FAIL") == .fail)
        #expect(LogStreamer.classify(line: "error: something") == .error)
        #expect(LogStreamer.classify(line: "warning: low") == .warning)
        #expect(LogStreamer.classify(line: "normal output") == .stdout)
    }

    @Test func classifyEdgeCases() {
        #expect(LogStreamer.classify(line: "fatal exception") == .error)
        #expect(LogStreamer.classify(line: "warn: deprecated") == .warning)
        #expect(LogStreamer.classify(line: "success: completed") == .pass)
        #expect(LogStreamer.classify(line: "timeout occurred") == .fail)
        #expect(LogStreamer.classify(line: "invalid_exe detected") == .fail)
        #expect(LogStreamer.classify(line: "c0000005 crash") == .error)
    }
}
