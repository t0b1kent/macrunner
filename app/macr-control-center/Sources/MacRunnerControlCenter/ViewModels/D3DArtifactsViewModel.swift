import SwiftUI

@MainActor
final class D3DArtifactsViewModel: ObservableObject {
    @Published var launcherResult: LauncherResult?
    @Published var traceLines: [String] = []
    @Published var reportJSON: String = ""
    @Published var ppmImage: NSImage?
    @Published var irJSON: String = ""
    @Published var compareResult: LauncherResult?
    @Published var compareTraceLines: [String] = []
    @Published var compareReportJSON: String = ""
    @Published var comparePpmImage: NSImage?
    @Published var compareIrJSON: String = ""
    @Published var traceFilter = ""
    @Published var isComparing = false

    var filteredTraceLines: [String] {
        if traceFilter.isEmpty { return traceLines }
        return traceLines.filter { $0.localizedCaseInsensitiveContains(traceFilter) }
    }

    var callCounts: [(String, Int)] {
        var counts: [String: Int] = [:]
        for line in traceLines {
            let prefix = line.prefix { $0 != "{" && $0 != "[" && $0 != "(" }
            let key = String(prefix).trimmingCharacters(in: .whitespaces)
            if !key.isEmpty {
                counts[key, default: 0] += 1
            }
        }
        return counts.sorted { $0.value > $1.value }.prefix(20).map { ($0.key, $0.value) }
    }

    func load(from result: LauncherResult?) {
        launcherResult = result
        traceLines = []
        reportJSON = ""
        ppmImage = nil
        irJSON = ""

        guard let r = result else { return }

        if let trace = r.d3dTracePath, let data = try? String(contentsOfFile: trace, encoding: .utf8) {
            traceLines = data.split(separator: "\n").map(String.init)
        }
        if let report = r.d3dReportPath, let data = try? Data(contentsOf: URL(fileURLWithPath: report)),
           let obj = try? JSONSerialization.jsonObject(with: data),
           let pretty = try? JSONSerialization.data(withJSONObject: obj, options: .prettyPrinted) {
            reportJSON = String(data: pretty, encoding: .utf8) ?? ""
        }
        if let ppm = r.d3dPpmPath, let data = try? Data(contentsOf: URL(fileURLWithPath: ppm)) {
            ppmImage = PPMParser.parse(data: data)
        }
        if let ir = r.d3dIrPath, let data = try? Data(contentsOf: URL(fileURLWithPath: ir)),
           let obj = try? JSONSerialization.jsonObject(with: data),
           let pretty = try? JSONSerialization.data(withJSONObject: obj, options: .prettyPrinted) {
            irJSON = String(data: pretty, encoding: .utf8) ?? ""
        }
    }

    func loadCompare(from result: LauncherResult?) {
        compareResult = result
        compareTraceLines = []
        compareReportJSON = ""
        comparePpmImage = nil
        compareIrJSON = ""

        guard let r = result else { return }

        if let trace = r.d3dTracePath, let data = try? String(contentsOfFile: trace, encoding: .utf8) {
            compareTraceLines = data.split(separator: "\n").map(String.init)
        }
        if let report = r.d3dReportPath, let data = try? Data(contentsOf: URL(fileURLWithPath: report)),
           let obj = try? JSONSerialization.jsonObject(with: data),
           let pretty = try? JSONSerialization.data(withJSONObject: obj, options: .prettyPrinted) {
            compareReportJSON = String(data: pretty, encoding: .utf8) ?? ""
        }
        if let ppm = r.d3dPpmPath, let data = try? Data(contentsOf: URL(fileURLWithPath: ppm)) {
            comparePpmImage = PPMParser.parse(data: data)
        }
        if let ir = r.d3dIrPath, let data = try? Data(contentsOf: URL(fileURLWithPath: ir)),
           let obj = try? JSONSerialization.jsonObject(with: data),
           let pretty = try? JSONSerialization.data(withJSONObject: obj, options: .prettyPrinted) {
            compareIrJSON = String(data: pretty, encoding: .utf8) ?? ""
        }
    }

    func clear() {
        launcherResult = nil
        compareResult = nil
        isComparing = false
        traceFilter = ""
        load(from: nil)
        loadCompare(from: nil)
    }
}
