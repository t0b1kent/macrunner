import SwiftUI

@main
struct MacRunnerControlCenterApp: App {
    init() {
        _ = ControlCenterCLI.handleIfNeeded()
        SparkleBootstrap.startIfAvailable()
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
                .frame(minWidth: 1024, minHeight: 700)
        }
        .windowResizability(.contentSize)
        .defaultSize(width: 1280, height: 800)
    }
}
