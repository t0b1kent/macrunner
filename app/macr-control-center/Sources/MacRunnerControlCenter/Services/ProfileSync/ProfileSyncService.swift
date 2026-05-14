import Foundation

struct ProfileSyncPlan: Codable, Equatable {
    var cachePath: String
    var pullInvocation: ProcessInvocation
    var submitEndpoint: URL
}

struct ProfileSyncService {
    var repoURL = URL(string: "https://github.com/MacRunner/profile-db.git")!
    var submitEndpoint = URL(string: "https://api.github.com/repos/MacRunner/profile-db/pulls")!

    func plan(root: String) -> ProfileSyncPlan {
        let cache = URL(fileURLWithPath: root).appendingPathComponent("cache/profile-db", isDirectory: true)
        let args: [String]
        if FileManager.default.fileExists(atPath: cache.appendingPathComponent(".git").path) {
            args = ["git", "-C", cache.path, "pull", "--ff-only"]
        } else {
            args = ["git", "clone", repoURL.absoluteString, cache.path]
        }
        return ProfileSyncPlan(cachePath: cache.path, pullInvocation: ProcessInvocation(executable: "/usr/bin/env", arguments: args, currentDirectory: root, environment: [:]), submitEndpoint: submitEndpoint)
    }
}
