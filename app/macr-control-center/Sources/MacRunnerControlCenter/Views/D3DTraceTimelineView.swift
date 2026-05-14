import SwiftUI

struct D3DTraceTimelineView: View {
    let lines: [String]

    var parsed: [TraceEvent] {
        lines.compactMap { line in
            guard let data = line.data(using: .utf8),
                  let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
            return TraceEvent(
                ts: json["ts"] as? String ?? "",
                call: json["call"] as? String ?? "",
                rc: json["rc"] as? Int ?? 0
            )
        }
    }

    var body: some View {
        if parsed.isEmpty {
            ContentUnavailableView("No trace data", systemImage: "list.bullet.indent")
        } else {
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 4) {
                    ForEach(parsed) { e in
                        HStack {
                            Text(e.ts).font(.caption2).foregroundStyle(.secondary).frame(width: 120, alignment: .leading)
                            Text(e.call).font(.caption).monospaced()
                            Spacer()
                            Text("rc=\(e.rc)").font(.caption2).foregroundStyle(e.rc == 0 ? .green : .red)
                        }
                    }
                }
                .padding()
            }
        }
    }
}

struct TraceEvent: Identifiable {
    let id = UUID()
    let ts: String
    let call: String
    let rc: Int
}
