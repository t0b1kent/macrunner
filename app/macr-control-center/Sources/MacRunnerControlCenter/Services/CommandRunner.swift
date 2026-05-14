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

    private func execute(
        command: String,
        arguments: [String],
        workingDirectory: String?,
        environment: [String: String]?,
        timeout: TimeInterval
    ) async -> CommandResult {
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

        return await withCheckedContinuation { continuation in
            var resumed = false
            func resumeOnce(_ result: CommandResult) {
                guard !resumed else { return }
                resumed = true
                continuation.resume(returning: result)
            }

            let start = Date()
            var stdoutData = Data()
            var stderrData = Data()

            let stdoutHandle = outPipe.fileHandleForReading
            let stderrHandle = errPipe.fileHandleForReading

            stdoutHandle.readabilityHandler = { handle in
                let data = handle.availableData
                if data.isEmpty {
                    handle.readabilityHandler = nil
                } else {
                    stdoutData.append(data)
                }
            }

            stderrHandle.readabilityHandler = { handle in
                let data = handle.availableData
                if data.isEmpty {
                    handle.readabilityHandler = nil
                } else {
                    stderrData.append(data)
                }
            }

            process.terminationHandler = { [weak self] _ in
                // Allow readability handlers to drain remaining data
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
                    stdoutHandle.readabilityHandler = nil
                    stderrHandle.readabilityHandler = nil

                    let duration = Int(Date().timeIntervalSince(start) * 1000)
                    let stdout = String(data: stdoutData, encoding: .utf8) ?? ""
                    let stderr = String(data: stderrData, encoding: .utf8) ?? ""

                    Task { @MainActor in
                        self?.stdoutBuffer = stdout
                        self?.stderrBuffer = stderr
                    }

                    let status: CommandStatus
                    if process.terminationStatus == 15 || process.terminationStatus == 9 {
                        status = .timeout
                    } else if process.terminationStatus == 0 {
                        status = .success
                    } else {
                        status = .failed
                    }

                    resumeOnce(CommandResult(
                        status: status,
                        exitCode: Int(process.terminationStatus),
                        stdout: stdout,
                        stderr: stderr,
                        durationMs: duration,
                        command: "\(command) \(arguments.joined(separator: " "))"
                    ))
                }
            }

            do {
                try process.run()
            } catch {
                resumeOnce(CommandResult(
                    status: .failed,
                    exitCode: -1,
                    stdout: "",
                    stderr: error.localizedDescription,
                    durationMs: 0,
                    command: "\(command) \(arguments.joined(separator: " "))"
                ))
                return
            }

            // macOS may not throw or fire terminationHandler for missing executables
            if process.processIdentifier == 0 {
                stdoutHandle.readabilityHandler = nil
                stderrHandle.readabilityHandler = nil
                let duration = Int(Date().timeIntervalSince(start) * 1000)
                let stderr = "Failed to launch process: executable not found or not executable"
                Task { @MainActor in
                    self.stdoutBuffer = ""
                    self.stderrBuffer = stderr
                }
                resumeOnce(CommandResult(
                    status: .failed,
                    exitCode: -1,
                    stdout: "",
                    stderr: stderr,
                    durationMs: duration,
                    command: "\(command) \(arguments.joined(separator: " "))"
                ))
                return
            }

            // Timeout on main queue
            DispatchQueue.main.asyncAfter(deadline: .now() + timeout) {
                if process.isRunning {
                    process.terminate()
                    DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
                        if process.isRunning {
                            kill(process.processIdentifier, SIGKILL)
                        }
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
        task?.cancel()
        isRunning = false
    }
}
