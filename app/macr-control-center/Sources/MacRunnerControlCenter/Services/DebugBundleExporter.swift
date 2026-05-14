import Foundation

struct DebugBundleExporter {
    static func export(
        settings: AppSettings,
        lastLauncherPath: String? = nil,
        lastStdoutPath: String? = nil,
        lastStderrPath: String? = nil,
        doctorPath: String? = nil,
        verifyPath: String? = nil,
        d3dPath: String? = nil,
        includeFullLogs: Bool = false
    ) -> URL? {
        let dateFormatter = DateFormatter()
        dateFormatter.dateFormat = "yyyyMMdd-HHmmss"
        let stamp = dateFormatter.string(from: Date())
        let desktop = FileManager.default.urls(for: .desktopDirectory, in: .userDomainMask).first!
        let bundleURL = desktop.appendingPathComponent("MacRunner-Debug-Bundle-\(stamp)", isDirectory: true)

        do {
            try FileManager.default.createDirectory(at: bundleURL, withIntermediateDirectories: true)

            var manifest: [String: Any] = [
                "created_at": ISO8601DateFormatter().string(from: Date()),
                "macrunner_root": settings.macRunnerRoot,
                "bundle_version": 1
            ]

            let maxLogSize = includeFullLogs ? Int64.max : 2 * 1024 * 1024

            func copyFile(_ source: String?, destName: String) {
                guard let source = source, FileManager.default.fileExists(atPath: source) else {
                    manifest["missing_\(destName)"] = true
                    return
                }
                let dest = bundleURL.appendingPathComponent(destName)
                do {
                    let attrs = try FileManager.default.attributesOfItem(atPath: source)
                    let size = attrs[.size] as? Int64 ?? 0
                    if size <= maxLogSize {
                        try FileManager.default.copyItem(atPath: source, toPath: dest.path)
                        manifest[destName] = ["size": size, "included": true]
                    } else {
                        let head = try String(contentsOfFile: source, encoding: .utf8)
                            .prefix(65536)
                        try String(head).write(to: dest, atomically: true, encoding: .utf8)
                        manifest[destName] = ["size": size, "included": false, "reason": "truncated"]
                    }
                } catch {
                    manifest["error_\(destName)"] = error.localizedDescription
                }
            }

            copyFile(lastLauncherPath, destName: "last-launcher.json")
            copyFile(lastStdoutPath, destName: "last-stdout.log")
            copyFile(lastStderrPath, destName: "last-stderr.log")
            copyFile(doctorPath, destName: "doctor.json")
            copyFile(verifyPath, destName: "verify.json")
            copyFile(d3dPath, destName: "d3d.json")

            let gitDiffPath = bundleURL.appendingPathComponent("git-diff-stat.txt")
            let data = runProcess(command: "/usr/bin/git", arguments: ["diff", "--stat"], timeout: 3, workingDirectory: settings.macRunnerRoot)
            if let data = data {
                try data.write(to: gitDiffPath)
            }

            let manifestData = try JSONSerialization.data(withJSONObject: manifest, options: .prettyPrinted)
            try manifestData.write(to: bundleURL.appendingPathComponent("manifest.json"))

            return bundleURL
        } catch {
            return nil
        }
    }

    private static func runProcess(command: String, arguments: [String], timeout: TimeInterval, workingDirectory: String? = nil) -> Data? {
        return DispatchQueue(label: "com.macrunner.debugbundle.process").sync {
            let process = Process()
            process.executableURL = URL(fileURLWithPath: command)
            process.arguments = arguments
            if let wd = workingDirectory {
                process.currentDirectoryURL = URL(fileURLWithPath: wd)
            }
            let pipe = Pipe()
            process.standardOutput = pipe

            do {
                try process.run()
            } catch {
                return nil
            }

            DispatchQueue.global().asyncAfter(deadline: .now() + timeout) {
                if process.isRunning {
                    process.terminate()
                    DispatchQueue.global().asyncAfter(deadline: .now() + 0.5) {
                        if process.isRunning {
                            kill(process.processIdentifier, SIGKILL)
                        }
                    }
                }
            }

            process.waitUntilExit()
            return (try? pipe.fileHandleForReading.readToEnd()) ?? Data()
        }
    }
}
