import SwiftUI

struct ContentView: View {
    @StateObject private var settingsVM = SettingsViewModel()
    @AppStorage("macrunner.developerMode") private var developerMode: Bool = false
    @State private var showOnboarding = false

    var body: some View {
        Group {
            if developerMode {
                DeveloperDashboardView(developerMode: $developerMode)
                    .environmentObject(settingsVM)
            } else {
                HomeView(developerMode: $developerMode)
                    .environmentObject(settingsVM)
            }
        }
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

struct DeveloperDashboardView: View {
    @Environment(\.colorScheme) private var scheme
    @Binding var developerMode: Bool
    @State private var selected: DeveloperPane = .library

    var body: some View {
        HStack(spacing: 0) {
            DeveloperSidebarView(selected: $selected, developerMode: $developerMode)
            Divider().background(Theme.Palette.separator(scheme))
            detail
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .background(Theme.Palette.bgPrimary(scheme))
    }

    @ViewBuilder
    private var detail: some View {
        DeveloperPaneContainer(title: selected.title, symbol: selected.symbol) {
            paneContent
        }
    }

    @ViewBuilder
    private var paneContent: some View {
        switch selected {
        case .library: AppLibraryView()
        case .runPanel: RunPanelView()
        case .bottles: BottleManagerView()
        case .d3d: D3DArtifactsView()
        case .queue: TaskQueueView()
        case .blocks: IntegrationBlocksView()
        case .doctor: DoctorView()
        case .logs: LiveLogView()
        case .processes: WineProcessView()
        case .performance: PerformanceView()
        case .compatDB: CompatibilityDBView()
        case .corpus: CorpusView()
        case .trial: LocalTrialWizardView()
        case .packaging: AppPackagingView()
        case .debugBundle: DebugBundleView()
        case .releases: ReleaseManagerView()
        case .worlds: ControlCenterWorldsView()
        case .help: HelpCenterView()
        case .settings: SettingsView()
        }
    }
}
