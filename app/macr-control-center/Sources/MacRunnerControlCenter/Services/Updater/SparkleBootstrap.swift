import Foundation

#if canImport(Sparkle)
import Sparkle
#endif

enum SparkleBootstrap {
    private static var retainedController: AnyObject?

    @MainActor
    static func startIfAvailable() {
        #if canImport(Sparkle)
        retainedController = SPUStandardUpdaterController(startingUpdater: true, updaterDelegate: nil, userDriverDelegate: nil)
        #endif
    }
}
