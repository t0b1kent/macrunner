import Foundation

enum ControlCenterCLI {
    static func handleIfNeeded(arguments: [String] = CommandLine.arguments) -> Bool {
        guard arguments.count > 1 else { return false }
        switch arguments[1] {
        case "--print-tool-paths":
            let paths = StoreToolLocator.toolPaths()
            print("legendary=\(paths["legendary"] ?? "missing")")
            print("gogdl=\(paths["gogdl"] ?? "missing")")
            exit(0)
        case "--hud-synthetic":
            do {
                let count = try SyntheticPerfEmitter().run()
                print("wrote \(count) samples to \(AppSettings.defaultRoot)/run/hud.sock.log")
                exit(0)
            } catch {
                fputs("hud synthetic failed: \(error.localizedDescription)\n", stderr)
                exit(1)
            }
        case "--verify-license":
            let license = arguments.dropFirst(2).joined(separator: " ")
            do {
                let result = try LicenseVerifier().verifyLicenseString(license)
                print("OK, valid until \(result.expiresPrefix)")
                exit(0)
            } catch {
                fputs("license verification failed: \(licenseErrorMessage(error))\n", stderr)
                exit(1)
            }
        case "--dump-steam-library":
            do {
                let root = try SteamLibraryService.ensureFixtureIfNeeded()
                try printJSON(SteamLibraryService(root: root).dump())
                exit(0)
            } catch { fail("steam library dump failed", error) }
        case "--epic-status":
            do { try printJSON(EpicLibraryService().status()); exit(0) } catch { fail("epic status failed", error) }
        case "--dump-epic-library":
            do { try printJSON(EpicLibraryService().dumpLibrary()); exit(0) } catch { fail("epic library dump failed", error) }
        case "--gog-status":
            do { try printJSON(GOGLibraryService().status()); exit(0) } catch { fail("gog status failed", error) }
        case "--dump-gog-library":
            do { try printJSON(GOGLibraryService().dumpLibrary()); exit(0) } catch { fail("gog library dump failed", error) }
        case "--dump-bnet-library":
            do { try printJSON(BattleNetProductDBReader().dump()); exit(0) } catch { fail("bnet library dump failed", error) }
        case "--import-exe":
            guard arguments.count > 2 else { failMessage("import-exe requires a path") }
            do { try printJSON(ManualImportWizard().importExecutable(path: arguments[2])); exit(0) } catch { fail("manual import failed", error) }
        case "--cover-cache-report":
            do { try printJSON(CoverCache().report()); exit(0) } catch { fail("cover cache report failed", error) }
        case "--bottle-dump":
            guard arguments.count > 2 else { failMessage("bottle-dump requires a bottle id") }
            do { try printJSON(BottleDeepSettingsService().loadOrCreate(bottleID: arguments[2])); exit(0) } catch { fail("bottle dump failed", error) }
        case "--program-overrides":
            guard arguments.count > 2 else { failMessage("program-overrides requires a bottle id") }
            do { try printJSON(ProgramOverrideService().list(bottleID: arguments[2])); exit(0) } catch { fail("program overrides failed", error) }
        case "--activity-export":
            do { try printJSON(ActivityDashboardService().snapshot()); exit(0) } catch { fail("activity export failed", error) }
        case "--cache-stats":
            do { try printJSON(CacheManagementService().stats()); exit(0) } catch { fail("cache stats failed", error) }
        case "--cache-export":
            guard arguments.count > 3 else { failMessage("cache-export requires <program-id> <destination>") }
            do {
                try CacheManagementService().export(programID: arguments[2], destination: URL(fileURLWithPath: arguments[3]))
                print("wrote \(arguments[3])")
                exit(0)
            } catch { fail("cache export failed", error) }
        case "--lint-localization":
            do {
                let result = try LocalizationLintService().lint()
                try printJSON(result)
                exit(result.missing.isEmpty ? 0 : 1)
            } catch { fail("localization lint failed", error) }
        case "--help-search":
            let query = arguments.dropFirst(2).joined(separator: " ")
            do { try printJSON(HelpCenterService().search(query).map { ["id": $0.id, "title": $0.title] }); exit(0) } catch { fail("help search failed", error) }
        case "--help-list":
            do { try printJSON(HelpCenterService().documents().map { ["id": $0.id, "title": $0.title] }); exit(0) } catch { fail("help list failed", error) }
        case "--bug-report-dry-run":
            do {
                let result = try InternalBugReportService().dryRun()
                try printJSON(["zip": result.zip.path, "sha256": result.sha256])
                exit(0)
            } catch { fail("bug report dry run failed", error) }
        default:
            return false
        }
    }

    private static func printJSON<T: Encodable>(_ value: T) throws {
        let data = try JSONEncoder.pretty.encode(value)
        FileHandle.standardOutput.write(data)
        FileHandle.standardOutput.write(Data("\n".utf8))
    }

    private static func fail(_ prefix: String, _ error: Error) -> Never {
        fputs("\(prefix): \(error.localizedDescription)\n", stderr)
        exit(1)
    }

    private static func failMessage(_ message: String) -> Never {
        fputs("\(message)\n", stderr)
        exit(2)
    }

    private static func licenseErrorMessage(_ error: Error) -> String {
        guard let licenseError = error as? LicenseError else { return error.localizedDescription }
        switch licenseError {
        case .invalidFormat, .invalidSignature:
            return "signature mismatch"
        case .expired, .invalidPublicKey:
            return licenseError.localizedDescription
        }
    }
}
