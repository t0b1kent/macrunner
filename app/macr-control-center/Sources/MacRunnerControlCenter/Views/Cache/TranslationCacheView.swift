import SwiftUI

struct TranslationCacheView: View {
    @State private var stats: CacheManagementStats?

    var body: some View {
        VStack(alignment: .leading) {
            Text("Translation Cache").font(.title.bold())
            if let stats {
                Text("Total: \(stats.totalSizeBytes) bytes")
                ForEach(stats.byProgram, id: \.programID) { row in
                    Text("\(row.programID): \(row.blockCount) blocks")
                }
            } else { ProgressView() }
        }
        .padding()
        .task { stats = try? CacheManagementService().stats() }
    }
}
