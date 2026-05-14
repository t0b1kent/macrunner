import SwiftUI

struct ReportReaderView: View {
    let report: Report?

    var body: some View {
        if let report = report {
            VStack(alignment: .leading, spacing: 0) {
                Text(report.title)
                    .font(.title2)
                    .padding()

                ScrollView {
                    VStack(alignment: .leading, spacing: 16) {
                        ForEach(report.sections.indices, id: \.self) { idx in
                            let section = report.sections[idx]
                            VStack(alignment: .leading, spacing: 8) {
                                Text(section.title)
                                    .font(.headline)
                                    .padding(.horizontal)

                                LazyVGrid(columns: [GridItem(.adaptive(minimum: 200))], spacing: 8) {
                                    ForEach(section.rows.indices, id: \.self) { rIdx in
                                        let row = section.rows[rIdx]
                                        ReportRow(label: row.label, value: row.value, status: row.status)
                                    }
                                }
                                .padding(.horizontal)
                            }
                        }
                    }
                    .padding(.vertical)
                }
            }
        } else {
            ContentUnavailableView("No report loaded", systemImage: "doc.text.magnifyingglass")
                .frame(maxHeight: .infinity)
        }
    }
}

private struct ReportRow: View {
    let label: String
    let value: String
    let status: ReportStatus

    var body: some View {
        HStack {
            Text(label)
                .font(.caption)
                .foregroundStyle(.secondary)
            Spacer()
            Text(value)
                .font(.callout)
                .foregroundStyle(color)
        }
        .padding(8)
        .background(Color(.controlBackgroundColor))
        .cornerRadius(6)
    }

    private var color: Color {
        switch status {
        case .pass: return .green
        case .fail: return .red
        case .warning: return .orange
        case .info: return .blue
        case .neutral: return .primary
        }
    }
}
