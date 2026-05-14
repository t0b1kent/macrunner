import Foundation

enum UpdateChannel: String, Codable, CaseIterable, Identifiable {
    case stable
    case beta
    case nightly

    var id: String { rawValue }
    var appcastURL: URL {
        URL(fileURLWithPath: AppSettings.defaultRoot)
            .appendingPathComponent("dist/appcast-\(rawValue).xml")
    }
    var warning: String? { self == .nightly ? "Nightly builds may introduce instability." : nil }
}

struct UpdaterConfiguration: Codable, Equatable {
    var channel: UpdateChannel
    var appcastURL: URL
    var sparkleEnabled: Bool
}

struct UpdaterService {
    func configuration(settings: AppSettings) -> UpdaterConfiguration {
        let channel = UpdateChannel(rawValue: settings.updateChannel ?? "stable") ?? .stable
        return UpdaterConfiguration(channel: channel, appcastURL: channel.appcastURL, sparkleEnabled: true)
    }
}
