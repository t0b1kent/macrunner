import AppKit
import Foundation

struct PurchaseService {
    var lifetimeURL = ProcessInfo.processInfo.environment["MACRUNNER_PURCHASE_URL"].flatMap(URL.init(string:))

    @MainActor
    func openLifetimePurchase() {
        guard let lifetimeURL else { return }
        NSWorkspace.shared.open(lifetimeURL)
    }
}

#if canImport(StoreKit)
import StoreKit
@available(macOS 12.0, *)
struct StoreKitPurchasePlan {
    let productID = "app.macrunner.lifetime"
    func products() async throws -> [Product] { try await Product.products(for: [productID]) }
}
#endif
