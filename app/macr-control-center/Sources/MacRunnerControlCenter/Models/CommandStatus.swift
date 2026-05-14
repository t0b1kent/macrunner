enum CommandStatus: String, Codable, Sendable {
    case running
    case success
    case failed
    case timeout
    case cancelled
}
