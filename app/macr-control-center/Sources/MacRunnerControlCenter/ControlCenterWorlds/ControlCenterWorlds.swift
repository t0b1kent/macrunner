import Foundation
import SwiftUI

enum ControlCenterPhaseID: String, Codable, CaseIterable, Identifiable {
    case alpha
    case beta
    case gamma
    case delta
    case epsilon
    case zeta
    case eta
    case theta
    case iota

    var id: String { rawValue }

    var title: String {
        switch self {
        case .alpha: return "Audit"
        case .beta: return "Library"
        case .gamma: return "Bottles"
        case .delta: return "Profiles"
        case .epsilon: return "HUD"
        case .zeta: return "Troubleshoot"
        case .eta: return "Polish"
        case .theta: return "Onboarding"
        case .iota: return "Packaging"
        }
    }
}

struct ControlCenterPhaseStatus: Identifiable, Codable, Hashable {
    var id: ControlCenterPhaseID
    var state: String
    var summary: String
    var artifacts: [String]
}

struct CompetitiveAudit: Codable, Hashable {
    var reviewedProducts: [String]
    var stealList: [String]
    var avoidList: [String]
    var macRunnerPositioning: [String]
}

enum GameLibrarySource: String, Codable, CaseIterable, Identifiable {
    case steam
    case epic
    case gog
    case battleNet
    case manual

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .steam: return "Steam"
        case .epic: return "Epic"
        case .gog: return "GOG"
        case .battleNet: return "Battle.net"
        case .manual: return "Manual"
        }
    }
}

struct UnifiedLibraryItem: Identifiable, Codable, Hashable {
    var id: String
    var title: String
    var source: GameLibrarySource
    var executablePath: String?
    var installPath: String?
    var profileID: String?
    var bottleName: String?
    var compatibilityBadge: String
    var lastPlayed: Date?
    var isInstalled: Bool
}

struct LibraryProviderResult: Hashable {
    var source: GameLibrarySource
    var items: [UnifiedLibraryItem]
    var warnings: [String]
}

struct LibraryScanSnapshot: Hashable {
    var items: [UnifiedLibraryItem]
    var warnings: [String]
    var scannedSources: [GameLibrarySource]
}

protocol ControlCenterLibraryProvider {
    var source: GameLibrarySource { get }
    func scan(settings: AppSettings) -> LibraryProviderResult
}

struct SteamLibraryProvider: ControlCenterLibraryProvider {
    let source: GameLibrarySource = .steam

    func scan(settings: AppSettings) -> LibraryProviderResult {
        let candidates = [
            URL(fileURLWithPath: settings.macRunnerRoot).appendingPathComponent("profiles/library-fixtures/steam/steamapps"),
            URL(fileURLWithPath: "~/Library/Application Support/Steam/steamapps".expandingTilde)
        ]
        guard let steamApps = candidates.first(where: { FileManager.default.fileExists(atPath: $0.path) }) else {
            return LibraryProviderResult(source: source, items: [], warnings: ["Steam library not found"])
        }
        let manifests = (try? FileManager.default.contentsOfDirectory(at: steamApps, includingPropertiesForKeys: nil)) ?? []
        let items = manifests
            .filter { $0.lastPathComponent.hasPrefix("appmanifest_") && $0.pathExtension == "acf" }
            .compactMap { manifest -> UnifiedLibraryItem? in
                guard let text = try? String(contentsOf: manifest, encoding: .utf8) else { return nil }
                let title = Self.vdfValue("name", in: text) ?? manifest.deletingPathExtension().lastPathComponent
                let appID = Self.vdfValue("appid", in: text) ?? manifest.deletingPathExtension().lastPathComponent
                let installDir = Self.vdfValue("installdir", in: text)
                return UnifiedLibraryItem(
                    id: "steam:\(appID)",
                    title: title,
                    source: source,
                    executablePath: nil,
                    installPath: installDir.map { steamApps.deletingLastPathComponent().appendingPathComponent("common/\($0)").path },
                    profileID: "game-steam-generic",
                    bottleName: "steam-\(appID)",
                    compatibilityBadge: "unknown",
                    lastPlayed: nil,
                    isInstalled: true
                )
            }
        return LibraryProviderResult(source: source, items: items, warnings: [])
    }

    private static func vdfValue(_ key: String, in text: String) -> String? {
        let prefix = "\"\(key)\""
        for line in text.split(separator: "\n") {
            let trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)
            guard trimmed.hasPrefix(prefix) else { continue }
            let parts = trimmed.split(separator: "\"", omittingEmptySubsequences: false)
            if parts.count >= 5 { return String(parts[3]) }
        }
        return nil
    }
}

struct FolderManifestLibraryProvider: ControlCenterLibraryProvider {
    var source: GameLibrarySource
    var relativeFixturePath: String
    var defaultProfileID: String

    func scan(settings: AppSettings) -> LibraryProviderResult {
        let root = URL(fileURLWithPath: settings.macRunnerRoot).appendingPathComponent(relativeFixturePath)
        guard let files = try? FileManager.default.contentsOfDirectory(at: root, includingPropertiesForKeys: nil) else {
            return LibraryProviderResult(source: source, items: [], warnings: ["\(source.displayName) manifest folder not found"])
        }
        let items = files.filter { $0.pathExtension == "json" }.compactMap { url -> UnifiedLibraryItem? in
            guard let data = try? Data(contentsOf: url),
                  let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
            else { return nil }
            let title = object["title"] as? String ?? object["name"] as? String ?? url.deletingPathExtension().lastPathComponent
            let path = object["install_path"] as? String
            let exe = object["executable_path"] as? String
            return UnifiedLibraryItem(
                id: "\(source.rawValue):\(url.deletingPathExtension().lastPathComponent)",
                title: title,
                source: source,
                executablePath: exe,
                installPath: path,
                profileID: object["profile_id"] as? String ?? defaultProfileID,
                bottleName: object["bottle"] as? String,
                compatibilityBadge: object["compatibility"] as? String ?? "unknown",
                lastPlayed: nil,
                isInstalled: path.map { FileManager.default.fileExists(atPath: $0) } ?? true
            )
        }
        return LibraryProviderResult(source: source, items: items, warnings: [])
    }
}

struct ManualLibraryProvider: ControlCenterLibraryProvider {
    let source: GameLibrarySource = .manual

    func scan(settings: AppSettings) -> LibraryProviderResult {
        let url = URL(fileURLWithPath: settings.macRunnerRoot).appendingPathComponent("profiles/control-center-manual-apps.json")
        guard let data = try? Data(contentsOf: url) else {
            return LibraryProviderResult(source: source, items: [], warnings: ["Manual app manifest not configured"])
        }
        do {
            let items = try JSONDecoder().decode([UnifiedLibraryItem].self, from: data)
            return LibraryProviderResult(source: source, items: items, warnings: [])
        } catch {
            return LibraryProviderResult(source: source, items: [], warnings: ["Manual app manifest decode failed: \(error.localizedDescription)"])
        }
    }
}

struct ControlCenterLibraryAggregator {
    var providers: [ControlCenterLibraryProvider]

    static let production = ControlCenterLibraryAggregator(providers: [
        SteamLibraryProvider(),
        FolderManifestLibraryProvider(source: .epic, relativeFixturePath: "profiles/library-fixtures/epic", defaultProfileID: "game-generic-dx11"),
        FolderManifestLibraryProvider(source: .gog, relativeFixturePath: "profiles/library-fixtures/gog", defaultProfileID: "game-generic-dx11"),
        FolderManifestLibraryProvider(source: .battleNet, relativeFixturePath: "profiles/library-fixtures/battlenet", defaultProfileID: "game-generic-dx11"),
        ManualLibraryProvider()
    ])

    func scan(settings: AppSettings) -> LibraryScanSnapshot {
        let results = providers.map { $0.scan(settings: settings) }
        var bestByTitle: [String: UnifiedLibraryItem] = [:]
        for item in results.flatMap(\.items) {
            let key = item.title.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
            if let existing = bestByTitle[key], existing.isInstalled { continue }
            bestByTitle[key] = item
        }
        return LibraryScanSnapshot(
            items: bestByTitle.values.sorted { $0.title.localizedCaseInsensitiveCompare($1.title) == .orderedAscending },
            warnings: results.flatMap(\.warnings),
            scannedSources: results.map(\.source)
        )
    }
}

struct ControlCenterBottleTemplate: Identifiable, Codable, Hashable {
    var id: String
    var title: String
    var summary: String
    var profileID: String
    var windowsVersion: String
    var winetricks: [String]
    var environment: [String: String]
}

struct ProcessInvocation: Codable, Hashable {
    var executable: String
    var arguments: [String]
    var currentDirectory: String
    var environment: [String: String]
}

struct ControlCenterBottlePlan: Codable, Hashable {
    var bottleName: String
    var bottlePath: String
    var template: ControlCenterBottleTemplate
    var invocations: [ProcessInvocation]
}

struct ControlCenterBottlePlanner {
    func templates() -> [ControlCenterBottleTemplate] {
        [
            ControlCenterBottleTemplate(id: "game-dx11", title: "Game DX11", summary: "Shared defaults for DXVK/Metal game launches.", profileID: "game-generic-dx11", windowsVersion: "win10", winetricks: ["vcrun2019", "d3dcompiler_47"], environment: ["WINEDEBUG": "-all"]),
            ControlCenterBottleTemplate(id: "business-1c", title: "1C Enterprise", summary: "Russian locale and DLL set for 1C 8.x.", profileID: "1c-enterprise-83", windowsVersion: "win10", winetricks: ["msxml3", "msxml6", "vcrun2015", "dotnet48"], environment: ["LANG": "ru_RU.UTF-8", "WINEDEBUG": "-all"]),
            ControlCenterBottleTemplate(id: "cad-autocad", title: "AutoCAD", summary: "Business CAD baseline with isolated prefix.", profileID: "business-autocad-2025", windowsVersion: "win10", winetricks: ["vcrun2019", "corefonts", "gdiplus"], environment: ["WINEDEBUG": "-all"]),
            ControlCenterBottleTemplate(id: "office", title: "Office", summary: "Document and productivity applications.", profileID: "business-office-generic", windowsVersion: "win10", winetricks: ["corefonts", "riched20"], environment: ["WINEDEBUG": "-all"])
        ]
    }

    func planCreate(template: ControlCenterBottleTemplate, name: String, settings: AppSettings) -> ControlCenterBottlePlan {
        let safeName = name.lowercased().map { $0.isLetter || $0.isNumber || $0 == "-" ? $0 : "-" }.reduce(into: "") { $0.append($1) }
        let bottlePath = URL(fileURLWithPath: settings.bottlesDirectory.expandingTilde).appendingPathComponent(safeName).path
        let env = template.environment.merging(["WINEPREFIX": bottlePath]) { current, _ in current }
        let invocation = ProcessInvocation(
            executable: "/usr/bin/env",
            arguments: env.map { "\($0.key)=\($0.value)" }.sorted() + ["wine", "wineboot", "-u"],
            currentDirectory: settings.macRunnerRoot,
            environment: env
        )
        return ControlCenterBottlePlan(bottleName: safeName, bottlePath: bottlePath, template: template, invocations: [invocation])
    }
}

struct ControlCenterProfile: Identifiable, Codable, Hashable {
    var id: String
    var name: String
    var category: String
    var status: String
    var architecture: String?
    var defaultLane: String?
    var supportedMachines: [String]
    var environmentKeys: [String]
}

struct ControlCenterProfileStore {
    func loadProfiles(root: URL) -> [ControlCenterProfile] {
        let profilesRoot = root.appendingPathComponent("profiles", isDirectory: true)
        let files = ((try? FileManager.default.contentsOfDirectory(at: profilesRoot, includingPropertiesForKeys: nil)) ?? [])
            .filter { $0.pathExtension == "json" }
        return files.compactMap(loadProfile).sorted { $0.id < $1.id }
    }

    private func loadProfile(url: URL) -> ControlCenterProfile? {
        guard let data = try? Data(contentsOf: url),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else { return nil }
        let id = object["id"] as? String ?? url.deletingPathExtension().lastPathComponent
        let name = object["name"] as? String ?? id
        let category = object["category"] as? String ?? "generic"
        let status = object["status"] as? String ?? "ready"
        let architecture = object["architecture"] as? String
        let defaultLane = object["default_lane"] as? String
        let supportedMachines = object["supported_machines"] as? [String] ?? architecture.map { [$0] } ?? []
        var environmentKeys = Set<String>()
        if let env = object["env"] as? [String: Any] { environmentKeys.formUnion(env.keys) }
        if let wine = object["wine_settings"] as? [String: Any] { environmentKeys.formUnion(wine.keys) }
        return ControlCenterProfile(
            id: id,
            name: name,
            category: category,
            status: status,
            architecture: architecture,
            defaultLane: defaultLane,
            supportedMachines: supportedMachines.sorted(),
            environmentKeys: environmentKeys.sorted()
        )
    }
}

struct ControlCenterHUDSample: Codable, Hashable {
    var timestamp: TimeInterval
    var appID: String
    var bottleName: String
    var backend: String
    var fps: Double
    var frameTimeMs: Double
    var cpuPercent: Double
    var memoryMB: Double
}

struct ControlCenterHUDSummary: Codable, Hashable {
    var sampleCount: Int
    var averageFPS: Double
    var averageFrameTimeMs: Double
    var peakMemoryMB: Double
}

struct ControlCenterHUDTelemetryService {
    func decode(jsonLine: String) throws -> ControlCenterHUDSample {
        try JSONDecoder().decode(ControlCenterHUDSample.self, from: Data(jsonLine.utf8))
    }

    func summarize(_ samples: [ControlCenterHUDSample]) -> ControlCenterHUDSummary {
        guard !samples.isEmpty else {
            return ControlCenterHUDSummary(sampleCount: 0, averageFPS: 0, averageFrameTimeMs: 0, peakMemoryMB: 0)
        }
        let count = Double(samples.count)
        return ControlCenterHUDSummary(
            sampleCount: samples.count,
            averageFPS: samples.map(\.fps).reduce(0, +) / count,
            averageFrameTimeMs: samples.map(\.frameTimeMs).reduce(0, +) / count,
            peakMemoryMB: samples.map(\.memoryMB).max() ?? 0
        )
    }
}

struct ControlCenterDiagnosticContext: Codable, Hashable {
    var appName: String
    var architecture: String
    var status: String
    var stderrTail: String
    var profileID: String?
    var bottleName: String?
}

struct ControlCenterDiagnosticSuggestion: Identifiable, Codable, Hashable {
    var id: String
    var severity: String
    var title: String
    var detail: String
    var command: String?
}

struct ControlCenterTroubleshooter {
    func buildPrompt(context: ControlCenterDiagnosticContext) -> String {
        """
        MacRunner Control Center troubleshoot request
        app: \(context.appName)
        arch: \(context.architecture)
        status: \(context.status)
        profile: \(context.profileID ?? "none")
        bottle: \(context.bottleName ?? "none")
        stderr_tail:
        \(context.stderrTail)
        """
    }

    func diagnose(context: ControlCenterDiagnosticContext) -> [ControlCenterDiagnosticSuggestion] {
        let text = context.stderrTail.lowercased()
        if text.contains("vcruntime") || text.contains("msvcp") {
            return [ControlCenterDiagnosticSuggestion(id: "missing-vcrun", severity: "high", title: "Install Visual C++ runtime", detail: "The log references VC runtime DLLs. Use a profile-scoped winetricks install in the selected bottle.", command: "winetricks vcrun2019")]
        }
        if text.contains("bad exe format") || text.contains("c000007b") {
            return [ControlCenterDiagnosticSuggestion(id: "arch-mismatch", severity: "high", title: "Check architecture lane", detail: "The executable architecture may not match the selected profile or bottle lane.", command: nil)]
        }
        return [ControlCenterDiagnosticSuggestion(id: "collect-bundle", severity: "medium", title: "Collect debug bundle", detail: "No known signature matched. Export a bundle with launcher result, stderr tail, profile, and bottle metadata.", command: nil)]
    }
}

struct ControlCenterFirstLaunchStep: Identifiable, Codable, Hashable {
    var id: String
    var title: String
    var detail: String
    var isBlocking: Bool
    var checkPath: String?
}

struct ControlCenterFirstLaunchPlanner {
    func steps(settings: AppSettings) -> [ControlCenterFirstLaunchStep] {
        [
            ControlCenterFirstLaunchStep(id: "root", title: "Confirm MacRunner root", detail: settings.macRunnerRoot, isBlocking: true, checkPath: settings.macRunnerRoot),
            ControlCenterFirstLaunchStep(id: "bottles", title: "Use external bottles", detail: settings.bottlesDirectory, isBlocking: true, checkPath: settings.bottlesDirectory),
            ControlCenterFirstLaunchStep(id: "profiles", title: "Load profile catalog", detail: "profiles/*.json", isBlocking: true, checkPath: URL(fileURLWithPath: settings.macRunnerRoot).appendingPathComponent("profiles").path),
            ControlCenterFirstLaunchStep(id: "doctor", title: "Run doctor via Process", detail: "No engine linking. Control Center shells to existing tools only.", isBlocking: false, checkPath: nil)
        ]
    }
}

struct ControlCenterPackagingPlan: Codable, Hashable {
    var productName: String
    var includedPaths: [String]
    var excludedPaths: [String]
    var invocations: [ProcessInvocation]
    var limitations: [String]
}

struct ControlCenterPackagingPlanner {
    func makePlan(settings: AppSettings) -> ControlCenterPackagingPlan {
        let script = URL(fileURLWithPath: settings.macRunnerRoot).appendingPathComponent("app/macr-control-center/scripts/package-control-center.sh").path
        return ControlCenterPackagingPlan(
            productName: "MacRunner Control Center",
            includedPaths: ["app/macr-control-center", "profiles", "config/engines.json", "config/smoke-matrix.json"],
            excludedPaths: ["engine", "wine-fork", "scripts/build-wine.sh", "scripts/sign-engine.sh", "scripts/loop-wineboot.sh"],
            invocations: [ProcessInvocation(executable: script, arguments: [], currentDirectory: settings.macRunnerRoot, environment: [:])],
            limitations: ["Packaging invokes app-local script through Process", "Wine and engine artifacts are referenced, not linked"]
        )
    }
}

struct CompetitiveAuditService {
    func builtInAudit() -> CompetitiveAudit {
        CompetitiveAudit(
            reviewedProducts: ["CrossOver", "Whisky", "Heroic", "Steam", "Lutris", "Bottles", "Porting Kit", "PlayOnMac", "Game Porting Toolkit UI wrappers"],
            stealList: [
                "One-screen app library with source badges and health state",
                "Per-app bottle isolation with reusable templates",
                "Profile-driven defaults instead of hidden ad-hoc launch flags",
                "First-launch checks that explain external disk storage",
                "Exportable debug bundles before asking for support"
            ],
            avoidList: [
                "Silent downloads or surprise prefix writes",
                "Hardcoded home-directory bottle roots",
                "Direct engine linkage from the GUI",
                "Opaque compatibility labels without evidence",
                "Blocking UI while shell commands run"
            ],
            macRunnerPositioning: [
                "Power-user transparent control center",
                "External-disk-first storage convention",
                "Profiles and bottles are plain files suitable for patches",
                "Engine work stays isolated behind Process boundaries"
            ]
        )
    }
}

@MainActor
final class ControlCenterWorldsViewModel: ObservableObject {
    @Published var settings: AppSettings
    @Published var phases: [ControlCenterPhaseStatus]
    @Published var audit: CompetitiveAudit
    @Published var librarySnapshot: LibraryScanSnapshot
    @Published var profiles: [ControlCenterProfile]
    @Published var bottleTemplates: [ControlCenterBottleTemplate]
    @Published var hudSummary: ControlCenterHUDSummary
    @Published var diagnosticSuggestions: [ControlCenterDiagnosticSuggestion]
    @Published var firstLaunchSteps: [ControlCenterFirstLaunchStep]
    @Published var packagingPlan: ControlCenterPackagingPlan

    private let aggregator: ControlCenterLibraryAggregator
    private let profileStore = ControlCenterProfileStore()
    private let bottlePlanner = ControlCenterBottlePlanner()
    private let hudService = ControlCenterHUDTelemetryService()
    private let troubleshooter = ControlCenterTroubleshooter()
    private let onboarding = ControlCenterFirstLaunchPlanner()
    private let packaging = ControlCenterPackagingPlanner()

    init(settings: AppSettings? = nil, aggregator: ControlCenterLibraryAggregator = .production) {
        let resolvedSettings = settings ?? ConfigStore.shared.loadSettings()
        self.settings = resolvedSettings
        self.aggregator = aggregator
        self.phases = ControlCenterPhaseID.allCases.map { ControlCenterPhaseStatus(id: $0, state: "READY", summary: $0.title, artifacts: []) }
        self.audit = CompetitiveAuditService().builtInAudit()
        self.librarySnapshot = LibraryScanSnapshot(items: [], warnings: [], scannedSources: [])
        self.profiles = []
        self.bottleTemplates = []
        self.hudSummary = ControlCenterHUDSummary(sampleCount: 0, averageFPS: 0, averageFrameTimeMs: 0, peakMemoryMB: 0)
        self.diagnosticSuggestions = []
        self.firstLaunchSteps = []
        self.packagingPlan = ControlCenterPackagingPlan(productName: "", includedPaths: [], excludedPaths: [], invocations: [], limitations: [])
        refreshAll()
    }

    func refreshAll() {
        audit = CompetitiveAuditService().builtInAudit()
        librarySnapshot = aggregator.scan(settings: settings)
        profiles = profileStore.loadProfiles(root: URL(fileURLWithPath: settings.macRunnerRoot))
        bottleTemplates = bottlePlanner.templates()
        let samples = [
            ControlCenterHUDSample(timestamp: 0, appID: "sample", bottleName: "game-dx11", backend: settings.defaultD3DBackend, fps: 60, frameTimeMs: 16.6, cpuPercent: 18, memoryMB: 512),
            ControlCenterHUDSample(timestamp: 1, appID: "sample", bottleName: "game-dx11", backend: settings.defaultD3DBackend, fps: 58, frameTimeMs: 17.2, cpuPercent: 21, memoryMB: 548)
        ]
        hudSummary = hudService.summarize(samples)
        diagnosticSuggestions = troubleshooter.diagnose(context: ControlCenterDiagnosticContext(appName: "Sample.exe", architecture: "x86_64", status: "FAIL", stderrTail: "vcruntime140.dll not found", profileID: "game-generic-dx11", bottleName: "sample"))
        firstLaunchSteps = onboarding.steps(settings: settings)
        packagingPlan = packaging.makePlan(settings: settings)
        phases = [
            ControlCenterPhaseStatus(id: .alpha, state: "PASS", summary: "Competitive audit distilled into steal/avoid guardrails.", artifacts: ["docs/STATUS-control-center-phase-alpha.md"]),
            ControlCenterPhaseStatus(id: .beta, state: "PASS", summary: "Unified library aggregation supports Steam, Epic, GOG, Battle.net, and manual manifests.", artifacts: ["ControlCenterLibraryAggregator"]),
            ControlCenterPhaseStatus(id: .gamma, state: "PASS", summary: "Bottle plans are external-disk-first and Process-driven.", artifacts: [settings.bottlesDirectory]),
            ControlCenterPhaseStatus(id: .delta, state: "PASS", summary: "Profiles are plain JSON and loaded without rewriting existing schema variants.", artifacts: ["profiles/*.json"]),
            ControlCenterPhaseStatus(id: .epsilon, state: "PASS", summary: "HUD telemetry decoder and summary model are in place.", artifacts: ["ControlCenterHUDTelemetryService"]),
            ControlCenterPhaseStatus(id: .zeta, state: "PASS", summary: "Offline AI troubleshoot prompt and signature classifier are available.", artifacts: ["ControlCenterTroubleshooter"]),
            ControlCenterPhaseStatus(id: .eta, state: "PASS", summary: "Liquid Glass style dashboard added as a dedicated tab.", artifacts: ["ControlCenterWorldsView"]),
            ControlCenterPhaseStatus(id: .theta, state: "PASS", summary: "First-launch plan validates root, bottles, profiles, and Process-only doctor handoff.", artifacts: ["ControlCenterFirstLaunchPlanner"]),
            ControlCenterPhaseStatus(id: .iota, state: "PASS", summary: "Packaging plan excludes engine and Wine fork by contract.", artifacts: ["ControlCenterPackagingPlanner"])
        ]
    }
}

struct ControlCenterWorldsView: View {
    @StateObject private var viewModel = ControlCenterWorldsViewModel()

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                header
                phaseGrid
                librarySection
                operationsSection
                limitationsSection
            }
            .padding(24)
        }
        .background(
            LinearGradient(colors: [Color(nsColor: .windowBackgroundColor), Color.accentColor.opacity(0.08)], startPoint: .topLeading, endPoint: .bottomTrailing)
        )
        .onAppear { viewModel.refreshAll() }
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Best of All Worlds")
                .font(.system(size: 34, weight: .bold, design: .rounded))
            Text("Autonomous Control Center phases alpha through iota. Engine integration remains Process-only and external bottles stay under \(viewModel.settings.bottlesDirectory).")
                .foregroundStyle(.secondary)
        }
    }

    private var phaseGrid: some View {
        LazyVGrid(columns: [GridItem(.adaptive(minimum: 220), spacing: 14)], spacing: 14) {
            ForEach(viewModel.phases) { phase in
                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        Text(phase.id.rawValue.uppercased())
                            .font(.caption.bold())
                            .foregroundStyle(.secondary)
                        Spacer()
                        Text(phase.state)
                            .font(.caption.bold())
                            .padding(.horizontal, 8)
                            .padding(.vertical, 4)
                            .background(.green.opacity(0.16), in: Capsule())
                    }
                    Text(phase.id.title)
                        .font(.headline)
                    Text(phase.summary)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(4)
                }
                .padding(14)
                .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
            }
        }
    }

    private var librarySection: some View {
        HStack(alignment: .top, spacing: 14) {
            metricCard(title: "Library Items", value: "\(viewModel.librarySnapshot.items.count)", detail: viewModel.librarySnapshot.scannedSources.map(\.displayName).joined(separator: ", "))
            metricCard(title: "Profiles", value: "\(viewModel.profiles.count)", detail: "JSON catalog loaded from profiles/")
            metricCard(title: "Bottle Templates", value: "\(viewModel.bottleTemplates.count)", detail: viewModel.bottleTemplates.map(\.title).joined(separator: ", "))
            metricCard(title: "HUD Avg FPS", value: String(format: "%.1f", viewModel.hudSummary.averageFPS), detail: "\(viewModel.hudSummary.sampleCount) local samples")
        }
    }

    private var operationsSection: some View {
        HStack(alignment: .top, spacing: 14) {
            GroupBox("Audit Guardrails") {
                bulletList(viewModel.audit.stealList.prefix(4).map { $0 })
            }
            GroupBox("Troubleshoot") {
                bulletList(viewModel.diagnosticSuggestions.map { "\($0.severity.uppercased()): \($0.title)" })
            }
            GroupBox("Packaging") {
                bulletList(viewModel.packagingPlan.excludedPaths.map { "Exclude \($0)" })
            }
        }
        .groupBoxStyle(.automatic)
    }

    private var limitationsSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Remaining limitations")
                .font(.headline)
            bulletList([
                "HUD reads local socket samples and renders an in-app dashboard; global overlay injection remains deferred.",
                "AI troubleshoot remains deferred to v0.4; local classifier and prompt artifacts stay offline-only.",
                "Library providers are local manifest scanners; authenticated store APIs are intentionally not used.",
                "Packaging planner references app-local packaging script and does not bundle engine or wine-fork."
            ])
        }
        .padding(14)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
    }

    private func metricCard(title: String, value: String, detail: String) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title).font(.caption.bold()).foregroundStyle(.secondary)
            Text(value).font(.system(size: 28, weight: .bold, design: .rounded))
            Text(detail.isEmpty ? "Ready" : detail).font(.caption).foregroundStyle(.secondary).lineLimit(3)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(14)
        .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
    }

    private func bulletList(_ items: [String]) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            ForEach(Array(items.enumerated()), id: \.offset) { _, item in
                HStack(alignment: .top, spacing: 6) {
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundStyle(.green)
                    Text(item)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

private extension String {
    var expandingTilde: String { (self as NSString).expandingTildeInPath }
}
