import Foundation
import SwiftUI

struct HelpDocument: Identifiable, Equatable {
    var id: String
    var title: String
    var markdown: String
}

struct HelpCenterService {
    func documents() -> [HelpDocument] {
        let files: [URL] = Bundle.module.urls(forResourcesWithExtension: "md", subdirectory: "Docs") ?? Bundle.module.urls(forResourcesWithExtension: "md", subdirectory: nil) ?? []
        var docs: [HelpDocument] = files.compactMap { url in
            guard let text = try? String(contentsOf: url, encoding: .utf8) else { return nil }
            let title = text.split(separator: "\n").first.map { String($0).replacingOccurrences(of: "# ", with: "") } ?? url.deletingPathExtension().lastPathComponent
            return HelpDocument(id: url.deletingPathExtension().lastPathComponent, title: title, markdown: text)
        }.sorted { $0.title < $1.title }
        if !docs.contains(where: { $0.id == "getting-started" }) {
            docs.append(HelpDocument(id: "getting-started", title: "Getting Started", markdown: "# Getting Started\n\nSet the MacRunner root, confirm external bottles, then connect store integrations."))
        }
        return docs
    }

    func search(_ query: String) -> [HelpDocument] {
        let docs = documents()
        guard !query.isEmpty else { return docs }
        return docs.filter { $0.title.localizedCaseInsensitiveContains(query) || $0.markdown.localizedCaseInsensitiveContains(query) }
    }
}

struct HelpCenterView: View {
    @State private var query = ""
    private let service = HelpCenterService()

    var body: some View {
        NavigationSplitView {
            List(service.search(query), id: \.id) { doc in
                Text(doc.title)
            }
            .searchable(text: $query)
            .navigationTitle("Help")
        } detail: {
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    ForEach(service.search(query), id: \.id) { doc in
                        Text((try? AttributedString(markdown: doc.markdown)) ?? AttributedString(doc.markdown))
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
                .padding()
            }
        }
    }
}
