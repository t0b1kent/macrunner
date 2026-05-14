import AppKit
import Foundation

struct PurchaseService {
    var lifetimeURL = URL(string: "https://buy.macrunner.app/?product=lifetime")!

    @MainActor
    func openLifetimePurchase() {
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
