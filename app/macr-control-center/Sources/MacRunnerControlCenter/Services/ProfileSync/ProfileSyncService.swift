import Foundation

struct ProfileSyncPlan: Codable, Equatable {
    var cachePath: String
    var pullInvocation: ProcessInvocation
    var submitEndpoint: String?
}

struct ProfileSyncService {
    var repoPath: String? = ProcessInfo.processInfo.environment["MACRUNNER_PRIVATE_PROFILE_REPO"]
    var submitEndpoint: String? = nil

    func plan(root: String) -> ProfileSyncPlan {
        let cache = URL(fileURLWithPath: root).appendingPathComponent("cache/profile-db", isDirectory: true)
        let args: [String]
        if FileManager.default.fileExists(atPath: cache.appendingPathComponent(".git").path) {
            args = ["git", "-C", cache.path, "pull", "--ff-only"]
        } else if let repoPath, !repoPath.isEmpty {
            args = ["git", "clone", repoPath, cache.path]
        } else {
            args = ["mkdir", "-p", cache.path]
        }
        return ProfileSyncPlan(cachePath: cache.path, pullInvocation: ProcessInvocation(executable: "/usr/bin/env", arguments: args, currentDirectory: root, environment: [:]), submitEndpoint: submitEndpoint)
    }
}
