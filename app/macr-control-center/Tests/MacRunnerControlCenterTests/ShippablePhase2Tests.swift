import Foundation
import Testing
@testable import MacRunnerControlCenter

struct ShippablePhase2Tests {
    @Test func anthropicDiagnosisParsesStructuredJSON() throws {
        let text = "Here is the fix: {\"root_cause\":\"Missing VC runtime\",\"fix\":{\"kind\":\"verb\",\"payload\":{\"verb\":\"vcrun2019\"}},\"confidence\":0.91}"
        let diagnosis = try AnthropicClient.parseDiagnosis(from: text)
        #expect(diagnosis.fix.kind == "verb")
        #expect(diagnosis.fix.payload["verb"] == "vcrun2019")
    }

    @Test func aiFixApplierPlansWinetricksVerb() {
        let diagnosis = AnthropicDiagnosis(rootCause: "missing", fix: .init(kind: "verb", payload: ["verb": "vcrun2019"]), confidence: 0.9)
        let plan = AIFixApplier().plan(diagnosis: diagnosis, bottlePath: "/tmp/bottle", profileID: nil, settings: .default)
        #expect(plan.processInvocations.first?.arguments.contains("winetricks") == true)
        #expect(plan.processInvocations.first?.arguments.contains("vcrun2019") == true)
    }

    @Test func steamVDFParserReadsNestedObjects() throws {
        let node = try SteamVDFParser().parse("\"users\" { \"123\" { \"AccountName\" \"timur\" \"MostRecent\" \"1\" } }")
        #expect(node["users"]?["123"]?["AccountName"]?.stringValue == "timur")
    }

    @Test func steamLocalParserReadsManifest() throws {
        let root = try tempDir()
        let steamapps = root.appendingPathComponent("steamapps", isDirectory: true)
        try FileManager.default.createDirectory(at: steamapps, withIntermediateDirectories: true)
        try "\"AppState\" { \"appid\" \"620\" \"name\" \"Portal 2\" \"installdir\" \"Portal 2\" }".write(to: steamapps.appendingPathComponent("appmanifest_620.acf"), atomically: true, encoding: .utf8)
        let games = SteamLocalDataParser(steamRoot: root).readInstalledGames()
        #expect(games.first?.appID == "620")
        #expect(games.first?.libraryItem().id == "steam:620")
    }

    @Test func otherStoreParsersAreDeterministic() {
        let epic = LegendaryWrapper().parseListJSON(Data("[{\"app_name\":\"foo\",\"app_title\":\"Foo Game\",\"is_installed\":true}]".utf8))
        let gog = GogdlWrapper().parseListJSON(Data("[{\"id\":42,\"title\":\"GOG Game\",\"installed\":true}]".utf8))
        #expect(epic.first?.source == .epic)
        #expect(gog.first?.id == "gog:42")
    }

    @Test func strictProfileValidatorReportsMissingField() throws {
        let report = StrictProfileValidator().validate(data: Data("{\"id\":\"x\"}".utf8))
        #expect(!report.isValid)
        #expect(report.issues.contains { $0.field == "name" })
        let valid = Data("{\"id\":\"x\",\"name\":\"X\",\"category\":\"game\",\"arch\":\"x86_64\",\"windows_version\":\"win10\",\"required_dlls\":[],\"verified_versions\":[]}".utf8)
        #expect(StrictProfileValidator().validate(data: valid).isValid)
    }

    @Test func hudBufferAndConfigWork() {
        var buffer = HUDRollingBuffer(capacity: 2)
        buffer.append(HUDFrameSample(fps: 60, frameMs: 16, gpuMs: nil, cpuMs: nil, cacheHits: 9, cacheMisses: 1))
        buffer.append(HUDFrameSample(fps: 55, frameMs: 18, gpuMs: nil, cpuMs: nil, cacheHits: 8, cacheMisses: 2))
        buffer.append(HUDFrameSample(fps: 50, frameMs: 20, gpuMs: nil, cpuMs: nil, cacheHits: 7, cacheMisses: 3))
        #expect(buffer.samples.count == 2)
        #expect(HUDSocketConfig.default(settings: .default).socketPath.hasSuffix("run/hud.sock"))
    }

    @Test func bottleShareEncryptsAndDecrypts() throws {
        let service = BottleShareService()
        let data = Data("payload".utf8)
        let encrypted = try service.encrypt(data, passphrase: "secret")
        #expect(encrypted != data)
        let decrypted = try service.decrypt(encrypted, passphrase: "secret")
        #expect(decrypted == data)
    }

    @Test func cachePrewarmPlanUsesRootScript() {
        let plan = TranslationCacheService().prewarmPlan(programPath: "/tmp/game.exe", settings: .default)
        #expect(plan.currentDirectory == "/Users/timurtoby/Documents/MacRunner/Main/MacRunner")
        #expect(plan.arguments.contains("./scripts/run-windows-app.sh"))
    }

    @Test func updaterChannelsProduceAppcastURLs() {
        var settings = AppSettings.default
        settings.updateChannel = "nightly"
        let config = UpdaterService().configuration(settings: settings)
        #expect(config.channel == .nightly)
        #expect(config.appcastURL.absoluteString.contains("appcast-nightly.xml"))
    }

    @Test func licenseFormatAndTrialGate() {
        #expect(LicenseVerifier().isWellFormed("MR2SH-TEST1-LOCAL-00001"))
        #expect(!LicenseVerifier().isWellFormed("bad"))
        #expect(LicenseStore().isFeatureAllowed("bottle_share"))
    }

    @Test func telemetrySanitizesSensitiveData() {
        let service = TelemetryService()
        let text = service.sanitize("/Users/alice/game.exe crashed user@example.com", home: "/Users/alice")
        #expect(text.contains("~/game.exe"))
        #expect(text.contains("<redacted-email>"))
    }

    @Test func helpDocsArePackaged() {
        let docs = HelpCenterService().documents()
        #expect(docs.contains { $0.id == "getting-started" })
    }

    private func tempDir() throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("macr-phase2-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }
}
