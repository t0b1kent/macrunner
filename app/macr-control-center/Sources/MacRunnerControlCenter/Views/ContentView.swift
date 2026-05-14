import SwiftUI

struct ContentView: View {
    @StateObject private var settingsVM = SettingsViewModel()
    @State private var selectedTab = 0
    @State private var showOnboarding = false

    var body: some View {
        TabView(selection: $selectedTab) {
            AppLibraryView()
                .tabItem { Label("Apps", systemImage: "app.badge.fill") }
                .tag(0)
            RunPanelView()
                .tabItem { Label("Run", systemImage: "play.fill") }
                .tag(1)
            TaskQueueView()
                .tabItem { Label("Queue", systemImage: "list.bullet.rectangle") }
                .tag(2)
            LiveLogView()
                .tabItem { Label("Logs", systemImage: "doc.text.magnifyingglass") }
                .tag(3)
            D3DArtifactsView()
                .tabItem { Label("D3D", systemImage: "cube") }
                .tag(4)
            DoctorView()
                .tabItem { Label("Doctor", systemImage: "stethoscope") }
                .tag(5)
            BottleManagerView()
                .tabItem { Label("Bottles", systemImage: "archivebox") }
                .tag(6)
            WineProcessView()
                .tabItem { Label("Processes", systemImage: "cpu") }
                .tag(7)
            CorpusView()
                .tabItem { Label("Corpus", systemImage: "doc.text") }
                .tag(8)
            PerformanceView()
                .tabItem { Label("Perf", systemImage: "chart.bar") }
                .tag(9)
            IntegrationBlocksView()
                .tabItem { Label("Blocks", systemImage: "building.blocks") }
                .tag(10)
            CompatibilityDBView()
                .tabItem { Label("Compat", systemImage: "checkmark.seal.fill") }
                .tag(11)
            DebugBundleView()
                .tabItem { Label("Bundle", systemImage: "doc.zipper") }
                .tag(12)
            AppPackagingView()
                .tabItem { Label("Package", systemImage: "archivebox.fill") }
                .tag(13)
            ReleaseManagerView()
                .tabItem { Label("Releases", systemImage: "shippingbox") }
                .tag(14)
            LocalTrialWizardView()
                .tabItem { Label("Trial", systemImage: "sparkles") }
                .tag(15)
            ControlCenterWorldsView()
                .tabItem { Label("Worlds", systemImage: "sparkles.rectangle.stack") }
                .tag(16)
            HelpCenterView()
                .tabItem { Label("Help", systemImage: "questionmark.circle") }
                .tag(17)
            SettingsView()
                .tabItem { Label("Settings", systemImage: "gear") }
                .tag(18)
        }
        .environmentObject(settingsVM)
        .frame(minWidth: 1024, minHeight: 700)
        .sheet(isPresented: $showOnboarding) {
            OnboardingView(isPresented: $showOnboarding)
                .environmentObject(settingsVM)
        }
        .onAppear {
            let settings = ConfigStore.shared.loadSettings()
            if settings.macRunnerRoot.isEmpty || !FileManager.default.fileExists(atPath: settings.macRunnerRoot) {
                showOnboarding = true
            }
        }
    }
}
