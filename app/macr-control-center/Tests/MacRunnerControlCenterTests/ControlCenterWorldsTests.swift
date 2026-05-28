import Foundation
import Testing
@testable import MacRunnerControlCenter

private struct MockLibraryProvider: ControlCenterLibraryProvider {
    var source: GameLibrarySource
    var items: [UnifiedLibraryItem]
    var warnings: [String] = []

    func scan(settings: AppSettings) -> LibraryProviderResult {
        LibraryProviderResult(source: source, items: items, warnings: warnings)
    }
}

struct ControlCenterWorldsTests {
    @Test func competitiveAuditHasGuardrails() {
        let audit = CompetitiveAuditService().builtInAudit()
        #expect(audit.reviewedProducts.count >= 8)
        #expect(audit.stealList.contains { $0.contains("bottle") || $0.contains("Bottle") })
        #expect(audit.avoidList.contains { $0.contains("Hardcoded") })
    }

    @Test func libraryAggregatorDeduplicatesByTitle() {
        let installed = UnifiedLibraryItem(id: "steam:1", title: "Sample Game", source: .steam, executablePath: nil, installPath: "/tmp/game", profileID: "game-steam-generic", bottleName: "steam-1", compatibilityBadge: "unknown", lastPlayed: nil, isInstalled: true)
        let duplicate = UnifiedLibraryItem(id: "manual:1", title: "sample game", source: .manual, executablePath: nil, installPath: nil, profileID: nil, bottleName: nil, compatibilityBadge: "unknown", lastPlayed: nil, isInstalled: false)
        let aggregator = ControlCenterLibraryAggregator(providers: [
            MockLibraryProvider(source: .manual, items: [duplicate], warnings: ["manual warning"]),
            MockLibraryProvider(source: .steam, items: [installed])
        ])
        let snapshot = aggregator.scan(settings: .default)
        #expect(snapshot.items.count == 1)
        #expect(snapshot.items.first?.source == .steam)
        #expect(snapshot.warnings == ["manual warning"])
    }

    @Test func bottlePlannerUsesExternalDiskAndProcessInvocation() throws {
        let planner = ControlCenterBottlePlanner()
        let template = try #require(planner.templates().first { $0.id == "business-1c" })
        let plan = planner.planCreate(template: template, name: "1C Main", settings: .default)
        #expect(plan.bottlePath.hasPrefix("/Users/timurtoby/Documents/MacRunner/Main/MacRunner/bottles/"))
        #expect(plan.invocations.first?.executable == "/usr/bin/env")
        #expect(plan.invocations.first?.arguments.contains("wineboot" ) == true)
        #expect(plan.invocations.first?.currentDirectory == "/Users/timurtoby/Documents/MacRunner/Main/MacRunner")
    }

    @Test func profileStoreLoadsSchemaVariants() throws {
        let root = try temporaryRoot()
        let profiles = root.appendingPathComponent("profiles", isDirectory: true)
        try FileManager.default.createDirectory(at: profiles, withIntermediateDirectories: true)
        try """
        {"id":"game-test","name":"Game Test","category":"game","supported_machines":["x86_64"],"default_lane":"rosetta","env":{"WINEDEBUG":"-all"}}
        """.write(to: profiles.appendingPathComponent("game-test.json"), atomically: true, encoding: .utf8)
        try """
        {"id":"business-test","name":"Business Test","category":"business","status":"research","architecture":"x86_64","wine_settings":{"locale":"ru_RU.UTF-8"}}
        """.write(to: profiles.appendingPathComponent("business-test.json"), atomically: true, encoding: .utf8)

        let loaded = ControlCenterProfileStore().loadProfiles(root: root)
        #expect(loaded.count == 2)
        #expect(loaded.map(\.id).contains("game-test"))
        #expect(loaded.first { $0.id == "business-test" }?.environmentKeys == ["locale"])
    }

    @Test func hudTelemetrySummarizesSamples() throws {
        let service = ControlCenterHUDTelemetryService()
        let sample = try service.decode(jsonLine: "{\"timestamp\":1,\"appID\":\"a\",\"bottleName\":\"b\",\"backend\":\"metal\",\"fps\":50,\"frameTimeMs\":20,\"cpuPercent\":25,\"memoryMB\":1024}")
        let summary = service.summarize([sample, ControlCenterHUDSample(timestamp: 2, appID: "a", bottleName: "b", backend: "metal", fps: 70, frameTimeMs: 14, cpuPercent: 30, memoryMB: 1536)])
        #expect(summary.sampleCount == 2)
        #expect(summary.averageFPS == 60)
        #expect(summary.peakMemoryMB == 1536)
    }

    @Test func troubleshooterBuildsPromptAndClassifiesRuntimeDll() {
        let service = ControlCenterTroubleshooter()
        let context = ControlCenterDiagnosticContext(appName: "Legacy.exe", architecture: "x86_64", status: "FAIL", stderrTail: "vcruntime140.dll missing", profileID: "game", bottleName: "legacy")
        let prompt = service.buildPrompt(context: context)
        let suggestions = service.diagnose(context: context)
        #expect(prompt.contains("Legacy.exe"))
        #expect(suggestions.first?.id == "missing-vcrun")
        #expect(suggestions.first?.command == "winetricks vcrun2019")
    }

    @Test func onboardingAndPackagingRespectBoundaries() {
        let settings = AppSettings.default
        let steps = ControlCenterFirstLaunchPlanner().steps(settings: settings)
        let plan = ControlCenterPackagingPlanner().makePlan(settings: settings)
        #expect(steps.contains { $0.id == "bottles" && $0.detail == "/Users/timurtoby/Documents/MacRunner/Main/MacRunner/bottles" })
        #expect(plan.invocations.first?.executable.contains("app/macr-control-center/scripts/package-control-center.sh") == true)
        #expect(plan.excludedPaths.contains("engine"))
        #expect(plan.excludedPaths.contains("wine-fork"))
    }

    private func temporaryRoot() throws -> URL {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("control-center-worlds-tests-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        return root
    }
}
