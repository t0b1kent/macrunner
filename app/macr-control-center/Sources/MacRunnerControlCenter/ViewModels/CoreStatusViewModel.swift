import SwiftUI

@MainActor
final class CoreStatusViewModel: ObservableObject {
    @Published var status: CoreStatus = .unknown(root: "")
    @Published var isChecking = false

    func refresh(root: String) {
        isChecking = true
        status = CoreStatusService.check(root: root)
        isChecking = false
    }

    func checkScriptHealth(script: String, root: String) -> Bool {
        let path = "\(root)/\(script)"
        return FileManager.default.fileExists(atPath: path)
    }
}
