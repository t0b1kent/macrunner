import SwiftUI

struct BottleDeepSettingsView: View {
    let bottleID: String
    @State private var settings: BottleDeepSettings?

    var body: some View {
        Form {
            if let settings {
                Section("DLL Overrides") { ForEach(settings.dllOverrides.sorted(by: { $0.key < $1.key }), id: \.key) { Text("\($0.key): \($0.value)") } }
                Section("Environment") { ForEach(settings.envVars.sorted(by: { $0.key < $1.key }), id: \.key) { Text("\($0.key)=\($0.value)") } }
                Section("Graphics") { Text(settings.graphicsBackend) }
            } else { Text("No bottle settings loaded") }
        }
        .task { settings = try? BottleDeepSettingsService().loadOrCreate(bottleID: bottleID) }
    }
}
