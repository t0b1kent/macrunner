import SwiftUI
import UniformTypeIdentifiers

@MainActor
final class PerformanceViewModel: ObservableObject {
    @Published var perfData: [String: Any] = [:]
    @Published var runHistory: [AppRunHistory] = []
    @Published var stats = PerfStats()
    @Published var selectedBackend: String? = nil
    @Published var timeRange: TimeRange = .all

    var settings: AppSettings

    var filteredHistory: [AppRunHistory] {
        var result = runHistory
        if let backend = selectedBackend {
            result = result.filter { $0.d3dBackend == backend }
        }
        switch timeRange {
        case .day:
            let cutoff = Date().addingTimeInterval(-24 * 60 * 60)
            result = result.filter { $0.timestamp > cutoff }
        case .week:
            let cutoff = Date().addingTimeInterval(-7 * 24 * 60 * 60)
            result = result.filter { $0.timestamp > cutoff }
        case .month:
            let cutoff = Date().addingTimeInterval(-30 * 24 * 60 * 60)
            result = result.filter { $0.timestamp > cutoff }
        case .all:
            break
        }
        return result.sorted { $0.timestamp > $1.timestamp }
    }

    var backends: [String] {
        Array(Set(runHistory.compactMap { $0.d3dBackend })).sorted()
    }

    var durationTrend: [(Date, Int)] {
        filteredHistory.reversed().map { ($0.timestamp, $0.durationMs) }
    }

    var backendBreakdown: [(String, Int, Int, Int)] {
        let groups = Dictionary(grouping: filteredHistory) { $0.d3dBackend ?? "none" }
        return groups.map { (backend, runs) in
            let passes = runs.filter { $0.status == "PASS" }.count
            let fails = runs.filter { $0.status == "FAIL" || $0.status == "CRASH" }.count
            let avg = runs.isEmpty ? 0 : runs.map { $0.durationMs }.reduce(0, +) / runs.count
            return (backend, passes, fails, avg)
        }.sorted { $0.1 + $0.2 > $1.1 + $1.2 }
    }

    var histogram: [(Int, Int)] {
        let buckets = [0, 100, 500, 1000, 2000, 5000, 10000, 30000]
        var counts = Array(repeating: 0, count: buckets.count)
        for run in filteredHistory {
            let ms = run.durationMs
            for (i, threshold) in buckets.enumerated() {
                if ms >= threshold {
                    counts[i] += 1
                }
            }
        }
        return buckets.enumerated().map { (i, threshold) in
            let next = i < buckets.count - 1 ? buckets[i + 1] : Int.max
            let count = filteredHistory.filter { $0.durationMs >= threshold && $0.durationMs < next }.count
            return (threshold, count)
        }
    }

    init(settings: AppSettings) {
        self.settings = settings
    }

    func refresh() {
        let path = "\(settings.macRunnerRoot)/reports/performance.json"
        if let data = try? Data(contentsOf: URL(fileURLWithPath: path)),
           let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
            perfData = json
        }

        runHistory = CompatibilityStore.shared.loadHistory()
        computeStats()
    }

    func exportCSV() {
        var csv = "Status,DurationMs,D3DBackend,Timestamp\n"
        for h in filteredHistory {
            csv += "\(h.status),\(h.durationMs),\(h.d3dBackend ?? ""),\(ISO8601DateFormatter().string(from: h.timestamp))\n"
        }
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.plainText]
        panel.nameFieldStringValue = "macr-performance.csv"
        if panel.runModal() == .OK, let url = panel.url {
            try? csv.write(to: url, atomically: true, encoding: .utf8)
        }
    }

    func exportJSON() {
        let export: [String: Any] = [
            "runs": filteredHistory.count,
            "stats": [
                "crashes": stats.crashes,
                "timeouts": stats.timeouts,
                "avgMs": stats.avgMs,
                "successRate": stats.successRate,
                "totalRuns": stats.totalRuns
            ],
            "backendBreakdown": backendBreakdown.map { ["backend": $0.0, "pass": $0.1, "fail": $0.2, "avgMs": $0.3] }
        ]
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.json]
        panel.nameFieldStringValue = "macr-performance.json"
        if panel.runModal() == .OK, let url = panel.url,
           let data = try? JSONSerialization.data(withJSONObject: export, options: .prettyPrinted) {
            try? data.write(to: url)
        }
    }

    private func computeStats() {
        let hist = filteredHistory
        let crashes = hist.filter { $0.status == "CRASH" }.count
        let timeouts = hist.filter { $0.status == "TIMEOUT" }.count
        let passes = hist.filter { $0.status == "PASS" }.count
        let avg = hist.isEmpty ? 0 : hist.map { $0.durationMs }.reduce(0, +) / hist.count
        let successRate = hist.isEmpty ? 0.0 : Double(passes) / Double(hist.count)
        stats = PerfStats(
            crashes: crashes,
            timeouts: timeouts,
            avgMs: avg,
            successRate: successRate,
            totalRuns: hist.count
        )
    }
}

enum TimeRange: String, CaseIterable {
    case day = "24h"
    case week = "7d"
    case month = "30d"
    case all = "All"
}

struct PerfStats: Sendable {
    var crashes = 0
    var timeouts = 0
    var avgMs = 0
    var successRate: Double = 0.0
    var totalRuns = 0
}
