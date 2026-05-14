struct ProcessReport: Codable, Sendable {
    var processes: [WineProcess]?
}

struct WineProcess: Codable, Sendable, Identifiable {
    var id: String { "\(pid ?? 0)-\(command ?? "")" }
    var pid: Int?
    var command: String?
    var age: String?
    var ppid: Int?
    var matchedReason: String?
    var status: String?

    enum CodingKeys: String, CodingKey {
        case pid
        case command
        case age
        case ppid
        case matchedReason = "matched_reason"
        case status
    }
}
