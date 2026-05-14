import Foundation

struct DebugBundleExporterV2 {
    struct Options {
        var includeArtifacts = true
        var includeLogs = true
        var includeReports = true
        var includeCompatibility = true
        var includeManifest = true
        var includeProcessSnapshot = true
        var includeSystemInfo = true
        var includeGitDiff = true
        var redactPaths = true
        var maxFileSizeBytes = 2 * 1024 * 1024
    }

    static func export(
        settings: AppSettings,
        app: AppEntry?,
        result: LauncherResult?,
        compatibility: CompatibilityEntry?,
        manifest: RealAppManifest?,
        options: Options = Options()
    ) -> URL? {
        let df = DateFormatter()
        df.dateFormat = "yyyyMMdd-HHmmss"
        let stamp = df.string(from: Date())
        let desktop = FileManager.default.urls(for: .desktopDirectory, in: .userDomainMask).first!
        let bundle = desktop.appendingPathComponent("MacRunner-Debug-Bundle-v2-\(stamp)", isDirectory: true)

        let home = FileManager.default.homeDirectoryForCurrentUser.path
        let redactions = [
            (home, "~"),
            (settings.macRunnerRoot, "<MACRUNNER_ROOT>")
        ]

        func redact(_ text: String) -> String {
            guard options.redactPaths else { return text }
            var result = text
            for (original, replacement) in redactions {
                result = result.replacingOccurrences(of: original, with: replacement)
            }
            return result
        }

        do {
            try FileManager.default.createDirectory(at: bundle, withIntermediateDirectories: true)

            var manifestDict: [String: Any] = [
                "created_at": ISO8601DateFormatter().string(from: Date()),
                "macrunner_root": settings.macRunnerRoot,
                "bundle_version": 2,
                "options": [
                    "artifacts": options.includeArtifacts,
                    "logs": options.includeLogs,
                    "reports": options.includeReports,
                    "compatibility": options.includeCompatibility,
                    "manifest": options.includeManifest,
                    "process_snapshot": options.includeProcessSnapshot,
                    "system_info": options.includeSystemInfo,
                    "git_diff": options.includeGitDiff,
                    "redact_paths": options.redactPaths
                ]
            ]

            func copy(_ src: String?, name: String) {
                guard let src = src, FileManager.default.fileExists(atPath: src) else {
                    manifestDict["missing_\(name)"] = true
                    return
                }
                let dest = bundle.appendingPathComponent(name)
                do {
                    let attrs = try FileManager.default.attributesOfItem(atPath: src)
                    let size = attrs[.size] as? Int64 ?? 0
                    if size <= options.maxFileSizeBytes {
                        if options.redactPaths, let content = try? String(contentsOfFile: src, encoding: .utf8) {
                            try redact(content).write(to: dest, atomically: true, encoding: .utf8)
                        } else {
                            try FileManager.default.copyItem(atPath: src, toPath: dest.path)
                        }
                        manifestDict[name] = ["size": size, "included": true]
                    } else {
                        let head = try String(contentsOfFile: src, encoding: .utf8).prefix(65536)
                        try redact(String(head)).write(to: dest, atomically: true, encoding: .utf8)
                        manifestDict[name] = ["size": size, "included": false, "reason": "truncated"]
                    }
                } catch {
                    manifestDict["error_\(name)"] = error.localizedDescription
                }
            }

            if options.includeArtifacts {
                copy(result?.d3dTracePath, name: "d3d_trace.jsonl")
                copy(result?.d3dIrPath, name: "d3d_ir.json")
                copy(result?.d3dReportPath, name: "d3d_report.json")
                copy(result?.d3dPpmPath, name: "output.ppm")
            }
            if options.includeLogs {
                copy(result?.stdoutPath, name: "stdout.log")
                copy(result?.stderrPath, name: "stderr.log")
            }
            if options.includeReports {
                copy("\(settings.macRunnerRoot)/reports/macr-doctor.json", name: "doctor.json")
                copy("\(settings.macRunnerRoot)/reports/native-platform-verify.json", name: "verify.json")
            }
            if options.includeCompatibility, let c = compatibility {
                let cdata = try JSONEncoder().encode(c)
                try cdata.write(to: bundle.appendingPathComponent("compatibility.json"))
                manifestDict["compatibility"] = true
            }
            if options.includeManifest, let m = manifest {
                let mdata = try JSONEncoder().encode(m)
                try mdata.write(to: bundle.appendingPathComponent("manifest.json"))
                manifestDict["manifest"] = true
            }

            if options.includeProcessSnapshot {
                let data = runProcess(command: "/bin/ps", arguments: ["aux"], timeout: 2)
                if let data = data {
                    try data.write(to: bundle.appendingPathComponent("process_snapshot.txt"))
                    manifestDict["process_snapshot"] = true
                } else {
                    manifestDict["process_snapshot"] = false
                }
            }

            if options.includeSystemInfo {
                let sys: [String: String] = [
                    "system": ProcessInfo.processInfo.operatingSystemVersionString,
                    "machine": ProcessInfo.processInfo.machineHardwareName
                ]
                if let data = try? JSONSerialization.data(withJSONObject: sys, options: .prettyPrinted) {
                    try data.write(to: bundle.appendingPathComponent("system_info.json"))
                }
                manifestDict["system_info"] = true
            }

            if options.includeGitDiff {
                let data = runProcess(command: "/usr/bin/git", arguments: ["diff", "--stat"], timeout: 3, workingDirectory: settings.macRunnerRoot)
                if let data = data {
                    try data.write(to: bundle.appendingPathComponent("git-diff-stat.txt"))
                    manifestDict["git_diff"] = true
                } else {
                    manifestDict["git_diff"] = false
                }
            }

            let mdata = try JSONSerialization.data(withJSONObject: manifestDict, options: .prettyPrinted)
            try mdata.write(to: bundle.appendingPathComponent("bundle_manifest.json"))

            return bundle
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

extension ProcessInfo {
    var machineHardwareName: String {
        var sysinfo = utsname()
        uname(&sysinfo)
        return withUnsafePointer(to: &sysinfo.machine) {
            $0.withMemoryRebound(to: CChar.self, capacity: 1) {
                String(validatingUTF8: $0) ?? "unknown"
            }
        }
    }
}
