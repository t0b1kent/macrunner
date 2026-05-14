import Foundation

protocol ReportReader {
    func read(path: String) -> Report?
}

struct Report: Sendable {
    var title: String
    var sections: [ReportSection]
    var rawJSON: String?
}

struct ReportSection: Sendable {
    var title: String
    var rows: [(label: String, value: String, status: ReportStatus)]
}

enum ReportStatus: String, Sendable {
    case pass, fail, warning, info, neutral
}

struct DoctorReportReader: ReportReader {
    func read(path: String) -> Report? {
        guard let data = try? Data(contentsOf: URL(fileURLWithPath: path)),
              let doctor = try? JSONDecoder().decode(DoctorReport.self, from: data) else {
            return nil
        }

        var sections: [ReportSection] = []

        if let host = doctor.host {
            sections.append(ReportSection(title: "Host", rows: [
                ("System", host.system ?? "—", .neutral),
                ("Machine", host.machine ?? "—", .neutral),
                ("macOS", host.macos ?? "—", .neutral)
            ]))
        }

        if let tools = doctor.tools {
            sections.append(ReportSection(title: "Tools", rows: [
                ("Clang", tools.clang == true ? "Yes" : "No", tools.clang == true ? .pass : .fail),
                ("Python", tools.python ?? "—", .neutral)
            ]))
        }

        if let lanes = doctor.lanes {
            var rows: [(String, String, ReportStatus)] = []
            if let arm64 = lanes.arm64 {
                rows.append(("ARM64", arm64.exists == true ? "OK" : "Missing", arm64.exists == true ? .pass : .fail))
            }
            if let x64 = lanes.x64 {
                rows.append(("x64", x64.exists == true ? "OK" : "Missing", x64.exists == true ? .pass : .fail))
            }
            if let x86 = lanes.x86 {
                rows.append(("x86", x86.exists == true ? "OK" : "Missing", x86.exists == true ? .pass : .fail))
            }
            sections.append(ReportSection(title: "Lanes", rows: rows))
        }

        if let graphics = doctor.graphics {
            sections.append(ReportSection(title: "Graphics", rows: [
                ("Render Core", graphics.renderCore == true ? "OK" : "Missing", graphics.renderCore == true ? .pass : .fail),
                ("Metal Probe", graphics.metalProbePass == true ? "PASS" : "FAIL", graphics.metalProbePass == true ? .pass : .fail)
            ]))
        }

        if let scripts = doctor.scripts {
            sections.append(ReportSection(title: "Scripts", rows: [
                ("Run Windows App", scripts.runWindowsApp?.exists == true ? "OK" : "Missing", scripts.runWindowsApp?.exists == true ? .pass : .fail),
                ("D3D Smoke", scripts.d3dSmoke?.exists == true ? "OK" : "Missing", scripts.d3dSmoke?.exists == true ? .pass : .warning)
            ]))
        }

        return Report(title: "Doctor Report", sections: sections, rawJSON: nil)
    }
}

struct VerifyReportReader: ReportReader {
    func read(path: String) -> Report? {
        guard let data = try? Data(contentsOf: URL(fileURLWithPath: path)),
              let verify = try? JSONDecoder().decode(PlatformVerifyReport.self, from: data) else {
            return nil
        }

        let status: ReportStatus
        switch verify.status?.uppercased() {
        case "PASS": status = .pass
        case "FAIL": status = .fail
        default: status = .warning
        }

        return Report(title: "Platform Verify", sections: [
            ReportSection(title: "Result", rows: [
                ("Status", verify.status ?? "—", status)
            ])
        ], rawJSON: nil)
    }
}

struct LauncherReportReader: ReportReader {
    func read(path: String) -> Report? {
        guard let data = try? Data(contentsOf: URL(fileURLWithPath: path)),
              let result = try? JSONDecoder().decode(LauncherResult.self, from: data) else {
            return nil
        }

        var sections: [ReportSection] = []

        let status: ReportStatus
        switch result.status?.uppercased() {
        case "PASS": status = .pass
        case "FAIL": status = .fail
        case "CRASH": status = .fail
        case "TIMEOUT": status = .warning
        default: status = .neutral
        }

        sections.append(ReportSection(title: "Run", rows: [
            ("Status", result.status ?? "—", status),
            ("Duration", "\(result.durationMs ?? 0)ms", .neutral),
            ("Exit Code", "\(result.rc ?? result.exitCode ?? -1)", status),
            ("Arch", result.arch ?? result.machine ?? "—", .neutral)
        ]))

        if result.d3dEnabled == true {
            let d3dStatus: ReportStatus
            switch result.d3dStatus?.uppercased() {
            case "PASS": d3dStatus = .pass
            case "FAIL": d3dStatus = .fail
            default: d3dStatus = .warning
            }
            sections.append(ReportSection(title: "D3D", rows: [
                ("Enabled", "Yes", .pass),
                ("Backend", result.d3dBackend ?? "—", .neutral),
                ("Status", result.d3dStatus ?? "—", d3dStatus),
                ("Unsupported Calls", "\(result.d3dUnsupportedCalls ?? 0)", .neutral)
            ]))
        }

        return Report(title: "Launcher Result", sections: sections, rawJSON: nil)
    }
}
