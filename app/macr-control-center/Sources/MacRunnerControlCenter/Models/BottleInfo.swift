import Foundation

struct BottleInfo: Codable, Sendable, Identifiable, Hashable {
    static func == (lhs: BottleInfo, rhs: BottleInfo) -> Bool {
        lhs.id == rhs.id
    }

    func hash(into hasher: inout Hasher) {
        hasher.combine(id)
    }
    var id: String { name }
    var name: String
    var path: String
    var sizeBytes: Int64
    var modified: Date?
    var status: String?
    var windowsVersion: String?
}
