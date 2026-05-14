import SwiftUI

struct IRReportView: View {
    let title: String
    let json: String

    var body: some View {
        if json.isEmpty {
            ContentUnavailableView("No \(title) data", systemImage: "doc.text")
        } else {
            ScrollView {
                Text(json)
                    .font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled)
                    .padding()
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .background(Color(.textBackgroundColor))
        }
    }
}
