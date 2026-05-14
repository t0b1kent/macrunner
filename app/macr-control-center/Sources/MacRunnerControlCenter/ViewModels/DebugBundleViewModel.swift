import SwiftUI

@MainActor
final class DebugBundleViewModel: ObservableObject {
    @Published var recentBundles: [URL] = []
    @Published var options = DebugBundleExporterV2.Options()
    @Published var isExporting = false
    @Published var lastBundleURL: URL?

    var settings: AppSettings

    init(settings: AppSettings) {
        self.settings = settings
        loadRecentBundles()
    }

    func export(settings: AppSettings, app: AppEntry? = nil, result: LauncherResult? = nil, compatibility: CompatibilityEntry? = nil, manifest: RealAppManifest? = nil) {
        isExporting = true
        lastBundleURL = nil
        Task {
            let url = DebugBundleExporterV2.export(
                settings: settings,
                app: app,
                result: result,
                compatibility: compatibility,
                manifest: manifest,
                options: options
            )
            await MainActor.run {
                self.lastBundleURL = url
                self.isExporting = false
                if let url = url {
                    self.recentBundles.insert(url, at: 0)
                    if self.recentBundles.count > 10 {
                        self.recentBundles = Array(self.recentBundles.prefix(10))
                    }
                }
            }
        }
    }

    func exportFromTaskQueue(settings: AppSettings) {
        let task = TaskQueue.shared.tasks.first { $0.status == .success || $0.status == .failed }
        export(settings: settings)
    }

    func revealInFinder(_ url: URL) {
        NSWorkspace.shared.open(url.deletingLastPathComponent())
    }

    func removeFromRecent(_ url: URL) {
        recentBundles.removeAll { $0 == url }
    }

    func resetOptions() {
        options = DebugBundleExporterV2.Options()
    }

    private func loadRecentBundles() {
        let desktop = FileManager.default.urls(for: .desktopDirectory, in: .userDomainMask).first!
        guard let contents = try? FileManager.default.contentsOfDirectory(atPath: desktop.path) else { return }
        recentBundles = contents
            .filter { $0.hasPrefix("MacRunner-Debug-Bundle") }
            .map { desktop.appendingPathComponent($0) }
            .sorted { url1, url2 in
                let attr1 = try? FileManager.default.attributesOfItem(atPath: url1.path)
                let attr2 = try? FileManager.default.attributesOfItem(atPath: url2.path)
                let d1 = attr1?[.modificationDate] as? Date ?? .distantPast
                let d2 = attr2?[.modificationDate] as? Date ?? .distantPast
                return d1 > d2
            }
    }
}
