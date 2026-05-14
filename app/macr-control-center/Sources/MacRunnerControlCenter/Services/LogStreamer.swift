import Foundation

@MainActor
final class LogStreamer: ObservableObject {
    @Published var lines: [LogLine] = []
    @Published var isRunning = false
    @Published var elapsedMs: Int = 0
    @Published var autoScroll = true

    private var process: Process?
    private var startTime: Date?
    private var timer: Timer?

    struct LogLine: Identifiable, Sendable {
        let id = UUID()
        let text: String
        let kind: LogKind
        let timestamp: Date
    }

    enum LogKind: String, Sendable {
        case stdout
        case stderr
        case warning
        case error
        case pass
        case fail
        case info
    }

    func run(
        command: String,
        arguments: [String] = [],
        workingDirectory: String? = nil,
        environment: [String: String]? = nil,
        timeout: TimeInterval = 300
    ) {
        guard !isRunning else { return }
        isRunning = true
        lines = []
        elapsedMs = 0
        startTime = Date()

        timer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            Task { @MainActor in
                if let start = self?.startTime {
                    self?.elapsedMs = Int(Date().timeIntervalSince(start) * 1000)
                }
            }
        }

        let process = Process()
        process.executableURL = URL(fileURLWithPath: command)
        process.arguments = arguments
        if let wd = workingDirectory {
            process.currentDirectoryURL = URL(fileURLWithPath: wd)
        }
        if let env = environment {
            var merged = ProcessInfo.processInfo.environment
            for (k, v) in env { merged[k] = v }
            process.environment = merged
        }

        let outPipe = Pipe()
        let errPipe = Pipe()
        process.standardOutput = outPipe
        process.standardError = errPipe
        self.process = process

        let stdoutQueue = DispatchQueue(label: "com.macrunner.logstreamer.stdout")
        let stderrQueue = DispatchQueue(label: "com.macrunner.logstreamer.stderr")
        var stdoutBuffer = Data()
        var stderrBuffer = Data()

        let stdoutHandle = outPipe.fileHandleForReading
        let stderrHandle = errPipe.fileHandleForReading

        stdoutHandle.readabilityHandler = { handle in
            let data = handle.availableData
            if data.isEmpty {
                handle.readabilityHandler = nil
            } else {
                stdoutQueue.sync { stdoutBuffer.append(data) }
            }
        }

        stderrHandle.readabilityHandler = { handle in
            let data = handle.availableData
            if data.isEmpty {
                handle.readabilityHandler = nil
            } else {
                stderrQueue.sync { stderrBuffer.append(data) }
            }
        }

        process.terminationHandler = { [weak self] _ in
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [weak self] in
                stdoutHandle.readabilityHandler = nil
                stderrHandle.readabilityHandler = nil

                var newLines: [LogLine] = []
                let outData = stdoutQueue.sync { stdoutBuffer }
                let errData = stderrQueue.sync { stderrBuffer }
                if let text = String(data: outData, encoding: .utf8), !text.isEmpty {
                    for chunk in text.split(separator: "\n", omittingEmptySubsequences: false) {
                        let line = String(chunk)
                        newLines.append(LogLine(text: line, kind: LogStreamer.classify(line: line), timestamp: Date()))
                    }
                }
                if let text = String(data: errData, encoding: .utf8), !text.isEmpty {
                    for chunk in text.split(separator: "\n", omittingEmptySubsequences: false) {
                        let line = String(chunk)
                        newLines.append(LogLine(text: line, kind: .stderr, timestamp: Date()))
                    }
                }

                if let strongSelf = self {
                    Task { @MainActor in
                        strongSelf.lines.append(contentsOf: newLines)
                        strongSelf.finish()
                    }
                }
            }
        }

        do {
            try process.run()
        } catch {
            Task { @MainActor in
                self.lines.append(LogLine(text: "Failed to start: \(error.localizedDescription)", kind: .error, timestamp: Date()))
                self.finish()
            }
            return
        }

        // Timeout on main queue
        DispatchQueue.main.asyncAfter(deadline: .now() + timeout) { [weak process] in
            if let p = process, p.isRunning {
                p.terminate()
                DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { [weak process] in
                    if let p = process, p.isRunning {
                        kill(p.processIdentifier, SIGKILL)
                    }
                }
            }
        }
    }

    func cancel() {
        if let p = process, p.isRunning {
            p.terminate()
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
                if let p = self?.process, p.isRunning {
                    kill(p.processIdentifier, SIGKILL)
                }
            }
        }
        finish()
    }

    private func finish() {
        isRunning = false
        timer?.invalidate()
        timer = nil
    }

    nonisolated static func classify(line: String) -> LogKind {
        let lower = line.lowercased()
        if lower.contains("error:") || lower.contains("fatal") || lower.contains("crash") || lower.contains("c0000005") {
            return .error
        }
        if lower.contains("warning:") || lower.contains("warn") {
            return .warning
        }
        if lower.contains("pass") || lower.contains("success") {
            return .pass
        }
        if lower.contains("fail") || lower.contains("timeout") || lower.contains("invalid_exe") {
            return .fail
        }
        return .stdout
    }
}
