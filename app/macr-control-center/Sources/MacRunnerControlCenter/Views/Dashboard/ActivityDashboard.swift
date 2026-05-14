import SwiftUI

struct ActivityDashboardView: View {
    @State private var snapshot: ActivityDashboardSnapshot?

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Activity Dashboard").font(.title.bold())
            if let snapshot {
                Text("Installed Windows programs: \(snapshot.installedCount)")
                Text("Cache hit rate: \(snapshot.cache.hitRate, specifier: "%.2f")")
                ForEach(snapshot.programs, id: \.id) { program in
                    Text("\(program.name): \(program.launchCount) launches")
                }
            } else { ProgressView() }
        }
        .padding()
        .task { snapshot = try? ActivityDashboardService().snapshot() }
    }
}
