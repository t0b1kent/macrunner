import Foundation

#if canImport(Sparkle)
import Sparkle
#endif

enum SparkleBootstrap {
    private static var retainedController: AnyObject?

    @MainActor
    static func startIfAvailable() {
        #if canImport(Sparkle)
        guard hasFeedURL() else { return }
        retainedController = SPUStandardUpdaterController(
            startingUpdater: true,
            updaterDelegate: nil,
            userDriverDelegate: nil
        )
        #endif
    }

    @MainActor
    static func checkForUpdates() {
        #if canImport(Sparkle)
        guard hasFeedURL() else { return }
        if retainedController == nil {
            retainedController = SPUStandardUpdaterController(
                startingUpdater: true,
                updaterDelegate: nil,
                userDriverDelegate: nil
            )
        }
        (retainedController as? SPUStandardUpdaterController)?.checkForUpdates(nil)
        #endif
    }

    private static func hasFeedURL() -> Bool {
        if let url = Bundle.main.object(forInfoDictionaryKey: "SUFeedURL") as? String, !url.isEmpty {
            return true
        }
        return false
    }
}
