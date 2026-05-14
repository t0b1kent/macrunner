import Foundation

struct PathSafety {
    static func isSubpath(of parent: String, path: String) -> Bool {
        let parentUrl = URL(fileURLWithPath: (parent as NSString).standardizingPath)
        let pathUrl = URL(fileURLWithPath: (path as NSString).standardizingPath)
        let parentComponents = parentUrl.pathComponents
        let pathComponents = pathUrl.pathComponents
        guard pathComponents.count >= parentComponents.count else { return false }
        for i in 0..<parentComponents.count {
            if parentComponents[i] != pathComponents[i] { return false }
        }
        return true
    }

    static func sanitizeBottlePath(_ path: String, base: String) -> Bool {
        isSubpath(of: base, path: path)
    }
}
