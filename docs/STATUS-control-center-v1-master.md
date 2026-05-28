# Control Center v1.0 Product Grade Status

Generated: 2026-05-14

## Gate 1 build
```console
[0/1] Planning build
Building for debugging...
[0/5] Write swift-version--58304C5D6DBC2206.txt
[2/4] Emitting module MacRunnerControlCenter
Build complete! (2.09s)
```

## Gate 2 tests
```console
✔ Suite TaskQueueTests passed after 4.400 seconds.
✔ Test defaultSettingsValid() passed after 4.399 seconds.
✔ Test settingsPersistAfterOnboardingSimulation() passed after 4.399 seconds.
✔ Suite OnboardingTests passed after 4.401 seconds.
✔ Test addUpdateDelete() passed after 4.400 seconds.
◇ Test quickRunUpdatesAppStatus() started.
✔ Test defaultSettings() passed after 4.400 seconds.
✔ Test persistAndLoad() passed after 4.400 seconds.
✔ Suite SettingsTests passed after 4.402 seconds.
✔ Test appendAndLoad() passed after 4.397 seconds.
✔ Test removeTask() passed after 4.397 seconds.
✔ Test maxEntries() passed after 4.854 seconds.
✔ Suite RunHistoryStoreTests passed after 4.859 seconds.
✔ Test cancellationWorks() passed after 4.859 seconds.
✔ Test echoStreaming() passed after 4.912 seconds.
✔ Suite LogStreamerTests passed after 4.914 seconds.
✔ Test stderrCapture() passed after 4.964 seconds.
✔ Test echoTest() passed after 4.964 seconds.
✔ Test quickRunUpdatesAppStatus() passed after 0.768 seconds.
◇ Test runAppViewModelOnCompleteCalled() started.
✔ Test runAppViewModelOnCompleteCalled() passed after 0.307 seconds.
✔ Suite AppLibraryTests passed after 5.478 seconds.
✔ Test timeoutWorks() passed after 6.541 seconds.
✔ Suite CommandRunnerTests passed after 6.542 seconds.
✔ Test run with 94 tests in 22 suites passed after 6.543 seconds.
```

## Gate 3 library providers
```console
[
  {
    "appid" : "620",
    "cover_path" : "\/Users\/timurtoby\/Library\/Caches\/MacRunner\/covers\/steam\/620.png",
    "install_dir" : "\/Volumes\/MacOS\/MacRunner\/app\/macr-control-center\/Tests\/Fixtures\/steam-account\/steamapps\/common\/Portal 2",
    "installed" : true,
    "last_played" : "2024-03-09T16:00:00Z",
    "name" : "Portal 2 Fixture"
  }
]
{
  "logged_in" : false,
  "provider" : "epic",
  "tool_path" : "\/Volumes\/MacOS\/MacRunner\/app\/macr-control-center\/Sources\/MacRunnerControlCenter\/Resources\/tools\/legendary"
}
{
  "logged_in" : false,
  "provider" : "gog",
  "tool_path" : "\/Volumes\/MacOS\/MacRunner\/app\/macr-control-center\/Sources\/MacRunnerControlCenter\/Resources\/tools\/gogdl"
}
[

]
{
  "entries" : [
    {
      "id" : "620",
      "path" : "\/Users\/timurtoby\/Library\/Caches\/MacRunner\/covers\/steam\/620.png",
      "provider" : "steam",
      "sizeBytes" : 138051,
      "updatedAt" : "2026-05-14T00:54:09Z"
    }
  ],
  "limitBytes" : 524288000,
  "root" : "\/Users\/timurtoby\/Library\/Caches\/MacRunner\/covers",
  "totalSizeBytes" : 138051
}
```

## Gate 4 manual import
```console
{
  "arch" : "x86_64",
  "bottle_id" : "manual-test-pe-8c2a2f73",
  "executable_path" : "app\/macr-control-center\/Tests\/Fixtures\/test-pe.exe",
  "pe_hash" : "8c2a2f7328e85fc0868e174dd80057bbb9e136b5229d8d6f3ed316c67f37cd3b",
  "profile_id_or_null" : "generic-x86_64-rosetta",
  "suggested_name" : "test-pe"
}
```

## Gate 5 compat runner
```console
/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/compat-runs/20260514-111119
compat runner: 3/3 PASS
notepad-plus-plus: Notepad++ -> portable-fixture
libreoffice-viewer: Office Viewer -> manual-media-required
sumatra-pdf: SumatraPDF -> portable-fixture
7zip: 7-Zip -> portable-fixture
far-manager: FAR Manager -> portable-fixture
dry-run entries: 5/50
```

## Gate 6 compat report
```console
/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/compat-public
rows=50
rows 50
macrunner_only 21
```

## Gate 7 power user features
```console
{
  "bottle_id" : "manual-test-pe-8c2a2f73",
  "dll_overrides" : {
    "d3d11" : "native,builtin"
  },
  "env_vars" : {
    "WINEDEBUG" : "-all"
  },
  "graphics_backend" : "auto",
  "registry_tweaks" : {
    "HKCU\\Software\\Wine\\MacRunner" : "enabled"
  },
  "wine_version" : "bundled"
}
[
  {
    "base_profile" : "game-generic-dx11",
    "executable_name" : "fixture.exe",
    "overrides" : {
      "WINEDEBUG" : "-all",
      "graphics_backend" : "dxvk"
    },
    "pe_hash" : "fb3765beb98bdf345d118b8a1ed5591a3ba45105d1287f74048a0686d72ce943"
  }
]
{
    "cache": {
        "hit_rate": 0.95,
        "total_size_bytes": 19
    },
    "database_path": "/Users/timurtoby/Library/Application Support/MacRunner/activity.sqlite",
    "installed_count": 2,
    "last_launched": "2026-05-14T01:11:19Z",
    "programs": [
        {
            "id": "D7BB66A2-4446-46CB-84E9-BF0B6F272406",
            "launchCount": 1,
            "name": "QuickRunTest"
        },
        {
            "id": "ED485670-CEE4-445B-928C-2A29C37DB5D0",
            "launchCount": 1,
            "name": "QuickRunTest"
        }
    ]
}
{
  "by_program" : [
    {
      "block_count" : 1,
      "program_id" : "fixture",
      "size_bytes" : 19
    }
  ],
  "lru_evicted_count" : 0,
  "total_size_bytes" : 19
}
wrote /tmp/test.mrcache
MRCACHE1
```

## Gate 8 localization docs bug report
```console
{
  "locales" : [
    "en",
    "ru"
  ],
  "missing" : {

  },
  "status" : "PASS"
}
[
  {
    "id" : "one-c-guide",
    "title" : "1С guide"
  }
]
[
  {
    "id" : "one-c-guide",
    "title" : "1С guide"
  },
  {
    "id" : "bottles",
    "title" : "Bottles"
  },
  {
    "id" : "getting-started",
    "title" : "Getting Started"
  },
  {
    "id" : "performance-hud",
    "title" : "Performance HUD"
  },
  {
    "id" : "profiles",
    "title" : "Profiles"
  },
  {
    "id" : "release-notes",
    "title" : "Release Notes"
  },
  {
    "id" : "store-integrations",
    "title" : "Store Integrations"
  },
  {
    "id" : "troubleshooting",
    "title" : "Troubleshooting"
  }
]
{
  "sha256" : "1d9292fc6f4993307d629ed7e387cbeaa01f3cb1687d15050cf27deb3ee59f82",
  "zip" : "\/var\/folders\/sp\/jbp4ynb53310q0w1db69lrlm0000gn\/T\/bug-report-2026-05-14T01-11-19Z.zip"
}
Archive:  /var/folders/sp/jbp4ynb53310q0w1db69lrlm0000gn/T/bug-report-2026-05-14T01-11-19Z.zip
  Length      Date    Time    Name
---------  ---------- -----   ----
        0  05-14-2026 11:11   macrunner-bug-report-2026-05-14T01-11-19Z/
      111  05-14-2026 11:11   macrunner-bug-report-2026-05-14T01-11-19Z/diagnostic.md
       41  05-14-2026 11:11   macrunner-bug-report-2026-05-14T01-11-19Z/sanitised-wine-log.txt
      195  05-14-2026 11:11   macrunner-bug-report-2026-05-14T01-11-19Z/manifest.json
---------                     -------
      347                     4 files
```

## Gate 9 package DMG
```console

/Users/timurtoby/Documents/MacRunner/Main/MacRunner/app/macr-control-center/Sources/MacRunnerControlCenter/ViewModels/AppLibraryViewModel.swift:30:74: warning: left side of nil coalescing operator '??' has non-optional type 'Date', so the right side is never used
28 |             result.sort { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
29 |         case .lastRun:
30 |             result.sort { ($0.updatedAt ?? .distantPast) > ($1.updatedAt ?? .distantPast) }
   |                                                                          `- warning: left side of nil coalescing operator '??' has non-optional type 'Date', so the right side is never used
31 |         case .duration:
32 |             result.sort { ($0.lastDurationMs ?? 0) > ($1.lastDurationMs ?? 0) }

/Users/timurtoby/Documents/MacRunner/Main/MacRunner/app/macr-control-center/Sources/MacRunnerControlCenter/ViewModels/BottleManagerViewModel.swift:76:13: warning: initialization of immutable value 'base' was never used; consider replacing with assignment to '_' or removing it [#no-usage]
 74 |
 75 |     func deleteAllStale() {
 76 |         let base = (settings.bottlesDirectory as NSString).expandingTildeInPath
    |             `- warning: initialization of immutable value 'base' was never used; consider replacing with assignment to '_' or removing it [#no-usage]
 77 |         for bottle in bottles where bottle.status == "stale" {
 78 |             _ = delete(bottle, confirmed: true)

/Users/timurtoby/Documents/MacRunner/Main/MacRunner/app/macr-control-center/Sources/MacRunnerControlCenter/ViewModels/DebugBundleViewModel.swift:43:13: warning: initialization of immutable value 'task' was never used; consider replacing with assignment to '_' or removing it [#no-usage]
41 |
42 |     func exportFromTaskQueue(settings: AppSettings) {
43 |         let task = TaskQueue.shared.tasks.first { $0.status == .success || $0.status == .failed }
   |             `- warning: initialization of immutable value 'task' was never used; consider replacing with assignment to '_' or removing it [#no-usage]
44 |         export(settings: settings)
45 |     }

/Users/timurtoby/Documents/MacRunner/Main/MacRunner/app/macr-control-center/Sources/MacRunnerControlCenter/Views/LiveLogView.swift:47:22: warning: 'onChange(of:perform:)' was deprecated in macOS 14.0: Use `onChange` with a two or zero parameter action closure instead. [#DeprecatedDeclaration]
 45 |                     }
 46 |                     .padding(.vertical, 4)
 47 |                     .onChange(of: streamer.lines.count) { _ in
    |                      `- warning: 'onChange(of:perform:)' was deprecated in macOS 14.0: Use `onChange` with a two or zero parameter action closure instead. [#DeprecatedDeclaration]
 48 |                         if streamer.autoScroll, !paused, let last = filteredLines.last {
 49 |                             withAnimation { proxy.scrollTo(last.id, anchor: .bottom) }

[#DeprecatedDeclaration]: <https://docs.swift.org/compiler/documentation/diagnostics/deprecated-declaration>
[3/5] Write Objects.LinkFileList
[4/5] Linking MacRunnerControlCenter
Build complete! (31.79s)
/Users/timurtoby/Documents/MacRunner/Main/MacRunner/dist/stage/MacRunner.app: replacing existing signature
DMG: /Users/timurtoby/Documents/MacRunner/Main/MacRunner/dist/MacRunner-1.0.dmg
SHA256: fc8998b8f8773e533263cb0f4bdebd893524df49983697f9fe59b82288fb8a11
hdiutil: verify: checksum of "dist/MacRunner-1.0.dmg" is VALID
GPT Partition Data (Primary GPT Tabl: verified   CRC32 $DED11308
Checksumming  (Apple_Free : 3)…
                    (Apple_Free : 3): verified   CRC32 $00000000
Checksumming disk image (Apple_APFS : 4)…
         disk image (Apple_APFS : 4): verified   CRC32 $58C120E8
Checksumming  (Apple_Free : 5)…
                    (Apple_Free : 5): verified   CRC32 $00000000
Checksumming GPT Partition Data (Backup GPT Table : 6)…
GPT Partition Data (Backup GPT Table: verified   CRC32 $DED11308
Checksumming GPT Header (Backup GPT Header : 7)…
  GPT Header (Backup GPT Header : 7): verified   CRC32 $987C61F4
verified   CRC32 $79D9A635
--validated:/Users/timurtoby/Documents/MacRunner/Main/MacRunner/dist/stage/MacRunner.app/Contents/Frameworks/Sparkle.framework/Versions/Current/XPCServices/Downloader.xpc
--prepared:/Users/timurtoby/Documents/MacRunner/Main/MacRunner/dist/stage/MacRunner.app/Contents/Frameworks/Sparkle.framework/Versions/Current/XPCServices/Installer.xpc
--validated:/Users/timurtoby/Documents/MacRunner/Main/MacRunner/dist/stage/MacRunner.app/Contents/Frameworks/Sparkle.framework/Versions/Current/XPCServices/Installer.xpc
--validated:/Users/timurtoby/Documents/MacRunner/Main/MacRunner/dist/stage/MacRunner.app/Contents/Frameworks/Sparkle.framework/Versions/Current/.
--prepared:/Users/timurtoby/Documents/MacRunner/Main/MacRunner/dist/stage/MacRunner.app/Contents/Frameworks/CrashReporter.framework/Versions/Current/.
--validated:/Users/timurtoby/Documents/MacRunner/Main/MacRunner/dist/stage/MacRunner.app/Contents/Frameworks/CrashReporter.framework/Versions/Current/.
dist/stage/MacRunner.app: valid on disk
dist/stage/MacRunner.app: satisfies its Designated Requirement
LICENSES_PRESENT
ENGINE_README_PRESENT
```

## Gate 10 closed source lint
```console
PASS:no-hardcoded-public-endpoints-or-foundation-markers
```

## End Condition
- Gate 1 build: PASS
- Gate 2 tests: PASS, 94/94
- Gate 3 libraries: PASS
- Gate 4 manual import: PASS
- Gate 5 compat runner: PASS
- Gate 6 compat report: PASS, 50 rows
- Gate 7 power user: PASS
- Gate 8 localization/docs/bug report: PASS
- Gate 9 DMG/codesign/LICENSES: PASS
- Gate 10 closed-source lint: PASS
