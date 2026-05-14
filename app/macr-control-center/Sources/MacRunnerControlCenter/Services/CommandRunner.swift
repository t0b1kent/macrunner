import Foundation

@MainActor
final class CommandRunner: ObservableObject {
    @Published var isRunning = false
    @Published var stdoutBuffer = ""
    @Published var stderrBuffer = ""
    @Published var lastResult: CommandResult?

    private var process: Process?
    private var task: Task<CommandResult, Never>?

    func run(
        command: String,
        arguments: [String] = [],
        workingDirectory: String? = nil,
        environment: [String: String]? = nil,
        timeout: TimeInterval = 300
    ) {
        guard !isRunning else { return }
        isRunning = true
        stdoutBuffer = ""
        stderrBuffer = ""

        task = Task {
            let result = await execute(
                command: command,
                arguments: arguments,
                workingDirectory: workingDirectory,
                environment: environment,
                timeout: timeout
            )
            await MainActor.run {
                self.isRunning = false
                self.lastResult = result
            }
            return result
        }
    }

    private nonisolated func execute(
        command: String,
        arguments: [String],
        workingDirectory: String?,
        environment: [String: String]?,
        timeout: TimeInterval
    ) async -> CommandResult {
        await withCheckedContinuation { continuation in
            DispatchQueue.global(qos: .userInitiated).async {
                let start = Date()
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

                Task { @MainActor in
                    self.process = process
                }

                do {
                    try process.run()
                } catch {
                    let duration = Int(Date().timeIntervalSince(start) * 1000)
                    continuation.resume(returning: CommandResult(
                        status: .failed,
                        exitCode: -1,
                        stdout: "",
                        stderr: error.localizedDescription,
                        durationMs: duration,
                        command: "\(command) \(arguments.joined(separator: " "))"
                    ))
                    return
                }

                let stdoutGroup = DispatchGroup()
                var stdoutData = Data()
                var stderrData = Data()

                stdoutGroup.enter()
                DispatchQueue.global(qos: .utility).async {
                    stdoutData = outPipe.fileHandleForReading.readDataToEndOfFile()
                    stdoutGroup.leave()
                }

                stdoutGroup.enter()
                DispatchQueue.global(qos: .utility).async {
                    stderrData = errPipe.fileHandleForReading.readDataToEndOfFile()
                    stdoutGroup.leave()
                }

                let timeoutDeadline = Date().addingTimeInterval(timeout)
                var timedOut = false
                while process.isRunning {
                    if Date() >= timeoutDeadline {
                        timedOut = true
                        process.terminate()
                        Thread.sleep(forTimeInterval: 0.5)
                        if process.isRunning {
                            kill(process.processIdentifier, SIGKILL)
                        }
                        break
                    }
                    Thread.sleep(forTimeInterval: 0.02)
                }
                process.waitUntilExit()
                _ = stdoutGroup.wait(timeout: .now() + 2)

                let duration = Int(Date().timeIntervalSince(start) * 1000)
                let stdout = String(data: stdoutData, encoding: .utf8) ?? ""
                let stderr = String(data: stderrData, encoding: .utf8) ?? ""
                let status: CommandStatus
                if timedOut || process.terminationStatus == 15 || process.terminationStatus == 9 {
                    status = .timeout
                } else if process.terminationStatus == 0 {
                    status = .success
                } else {
                    status = .failed
                }

                Task { @MainActor in
                    self.stdoutBuffer = stdout
                    self.stderrBuffer = stderr
                }

                continuation.resume(returning: CommandResult(
                    status: status,
                    exitCode: Int(process.terminationStatus),
                    stdout: stdout,
                    stderr: stderr,
                    durationMs: duration,
                    command: "\(command) \(arguments.joined(separator: " "))"
                ))
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
        task?.cancel()
        isRunning = false
    }
}
