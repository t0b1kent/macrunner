import SwiftUI

struct ActivityDashboardView: View {
    @State private var snapshot: ActivityDashboardSnapshot?

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Activity Dashboard").font(.title.bold())
            if let snapshot {
                Text("Installed Windows programs: \(snapshot.installedCount)")
                if let owned = snapshot.steamOwnedGames {
                    Text("Steam owned games: \(owned)")
                }
                Text("Cache hit rate: \(snapshot.cache.hitRate, specifier: "%.2f")")
                if !snapshot.recentlyPlayed.isEmpty {
                    Text("Recently Played").font(.headline)
                    ScrollView(.horizontal) {
                        HStack {
                            ForEach(snapshot.recentlyPlayed, id: \.appid) { game in
                                VStack(alignment: .leading) {
                                    Text(game.name).font(.headline)
                                    Text("\(game.recentlyPlayedMinutes ?? 0) minutes in two weeks")
                                        .font(.caption)
                                        .foregroundStyle(.secondary)
                                }
                                .padding(10)
                                .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 12))
                            }
                        }
                    }
                }
                ForEach(snapshot.programs, id: \.id) { program in
                    Text("\(program.name): \(program.launchCount) launches")
                }
            } else { ProgressView() }
        }
        .padding()
        .task { snapshot = try? ActivityDashboardService().snapshot() }
    }
}
