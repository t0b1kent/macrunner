struct CommandResult: Codable, Sendable {
    var status: CommandStatus
    var exitCode: Int
    var stdout: String
    var stderr: String
    var durationMs: Int
    var command: String
}
