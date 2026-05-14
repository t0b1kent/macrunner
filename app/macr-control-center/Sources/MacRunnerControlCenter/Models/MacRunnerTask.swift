import Foundation

struct MacRunnerTask: Identifiable, Codable, Sendable, Hashable {
    static func == (lhs: MacRunnerTask, rhs: MacRunnerTask) -> Bool {
        lhs.id == rhs.id
    }

    func hash(into hasher: inout Hasher) {
        hasher.combine(id)
    }
    let id: UUID
    var type: TaskType
    var status: TaskStatus
    var command: String
    var arguments: [String]
    var workingDirectory: String?
    var environment: [String: String]?
    var timeout: TimeInterval
    var createdAt: Date
    var startedAt: Date?
    var completedAt: Date?
    var logs: [String]
    var exitCode: Int?
    var durationMs: Int?
    var artifactPaths: [String]
    var errorMessage: String?

    enum TaskType: String, Codable, Sendable {
        case runApp
        case runDoctor
        case runVerify
        case runD3DSmoke
        case runWinAPIMatrix
        case runRealAppCorpus
        case cleanupRuntime
        case exportDebugBundle
        case packageApp
        case customCommand
    }

    enum TaskStatus: String, Codable, Sendable {
        case queued
        case running
        case success
        case failed
        case cancelled
        case timeout
    }

    init(
        id: UUID = UUID(),
        type: TaskType,
        command: String,
        arguments: [String] = [],
        workingDirectory: String? = nil,
        environment: [String: String]? = nil,
        timeout: TimeInterval = 300
    ) {
        self.id = id
        self.type = type
        self.status = .queued
        self.command = command
        self.arguments = arguments
        self.workingDirectory = workingDirectory
        self.environment = environment
        self.timeout = timeout
        self.createdAt = Date()
        self.logs = []
        self.artifactPaths = []
    }
}
