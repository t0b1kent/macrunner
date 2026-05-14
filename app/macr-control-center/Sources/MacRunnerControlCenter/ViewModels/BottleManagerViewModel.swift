import SwiftUI

@MainActor
final class BottleManagerViewModel: ObservableObject {
    @Published var bottles: [BottleInfo] = []
    @Published var error: String?
    @Published var dryRunResults: [String]?
    @Published var selectedBottle: BottleInfo?
    @Published var isRepairing = false
    @Published var repairOutput = ""
    @Published var showRepairSheet = false

    var settings: AppSettings

    init(settings: AppSettings) {
        self.settings = settings
    }

    func refresh() {
        let base = (settings.bottlesDirectory as NSString).expandingTildeInPath
        var list: [BottleInfo] = []
        let fm = FileManager.default
        guard let contents = try? fm.contentsOfDirectory(atPath: base) else {
            bottles = []
            return
        }
        for name in contents {
            let path = "\(base)/\(name)"
            var isDir: ObjCBool = false
            guard fm.fileExists(atPath: path, isDirectory: &isDir), isDir.boolValue else { continue }
            var size: Int64 = 0
            if let enumerator = fm.enumerator(atPath: path) {
                for case let file as String in enumerator {
                    let full = "\(path)/\(file)"
                    if let attrs = try? fm.attributesOfItem(atPath: full) {
                        size += attrs[.size] as? Int64 ?? 0
                    }
                }
            }
            let modified = (try? fm.attributesOfItem(atPath: path)[.modificationDate] as? Date)
            let status = isStale(path: path) ? "stale" : "present"
            let windowsVersion = readWindowsVersion(path: path)
            list.append(BottleInfo(
                name: name,
                path: path,
                sizeBytes: size,
                modified: modified,
                status: status,
                windowsVersion: windowsVersion
            ))
        }
        bottles = list.sorted { $0.name < $1.name }
    }

    func delete(_ bottle: BottleInfo, confirmed: Bool) -> Bool {
        guard confirmed else { return false }
        let base = (settings.bottlesDirectory as NSString).expandingTildeInPath
        guard PathSafety.sanitizeBottlePath(bottle.path, base: base) else {
            error = "Refusing to delete path outside bottles directory"
            return false
        }
        do {
            try FileManager.default.removeItem(atPath: bottle.path)
            if selectedBottle?.id == bottle.id {
                selectedBottle = nil
            }
            refresh()
            return true
        } catch {
            self.error = error.localizedDescription
            return false
        }
    }

    func deleteAllStale() {
        let base = (settings.bottlesDirectory as NSString).expandingTildeInPath
        for bottle in bottles where bottle.status == "stale" {
            _ = delete(bottle, confirmed: true)
        }
        refresh()
    }

    func revealInFinder(_ bottle: BottleInfo) {
        NSWorkspace.shared.open(URL(fileURLWithPath: bottle.path))
    }

    func archive(_ bottle: BottleInfo, to name: String) {
        let base = (settings.bottlesDirectory as NSString).expandingTildeInPath
        guard PathSafety.sanitizeBottlePath(bottle.path, base: base) else {
            error = "Refusing to archive outside bottles directory"
            return
        }
        let dest = "\(base)/\(name)"
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/ditto")
        process.arguments = ["-ck", bottle.path, dest]
        try? process.run()
        process.waitUntilExit()
        if process.terminationStatus != 0 {
            error = "Archive failed"
        }
    }

    func repair(_ bottle: BottleInfo) {
        isRepairing = true
        repairOutput = ""
        showRepairSheet = true
        Task {
            let process = Process()
            process.executableURL = URL(fileURLWithPath: "/usr/bin/env")
            process.arguments = ["WINEPREFIX=\(bottle.path)", "wine", "wineboot", "-u"]
            process.currentDirectoryURL = URL(fileURLWithPath: settings.macRunnerRoot)
            let pipe = Pipe()
            process.standardOutput = pipe
            process.standardError = pipe
            do {
                try process.run()
                process.waitUntilExit()
                let data = pipe.fileHandleForReading.readDataToEndOfFile()
                let output = String(data: data, encoding: .utf8) ?? ""
                await MainActor.run {
                    self.repairOutput = output.isEmpty ? "Repair completed." : output
                    self.isRepairing = false
                }
            } catch {
                await MainActor.run {
                    self.repairOutput = "Repair failed: \(error.localizedDescription)"
                    self.isRepairing = false
                }
            }
        }
    }

    func dryRunCleanup() {
        let base = (settings.bottlesDirectory as NSString).expandingTildeInPath
        let fm = FileManager.default
        guard let contents = try? fm.contentsOfDirectory(atPath: base) else { return }
        var results: [String] = []
        for name in contents {
            let path = "\(base)/\(name)"
            if isStale(path: path) {
                results.append("Would remove stale: \(name)")
            }
        }
        dryRunResults = results.isEmpty ? ["Nothing to clean"] : results
    }

    private func isStale(path: String) -> Bool {
        guard let mod = (try? FileManager.default.attributesOfItem(atPath: path)[.modificationDate] as? Date) else { return false }
        let monthAgo = Date().addingTimeInterval(-30 * 24 * 60 * 60)
        return mod < monthAgo
    }

    private func readWindowsVersion(path: String) -> String? {
        let systemReg = "\(path)/system.reg"
        guard let data = try? String(contentsOfFile: systemReg, encoding: .utf8) else { return nil }
        for line in data.split(separator: "\n") {
            if line.contains("\"ProductName\"") {
                let parts = line.split(separator: "\"")
                if parts.count >= 4 {
                    return String(parts[3]).trimmingCharacters(in: .whitespaces)
                }
            }
        }
        return nil
    }
}
