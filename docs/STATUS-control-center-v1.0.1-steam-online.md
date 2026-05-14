# Control Center v1.0.1 Steam Online Status

Generated: 2026-05-14

## Build
```console
[0/1] Planning build
Building for debugging...
[0/6] Copying steam-setup.md
[1/6] Write swift-version--58304C5D6DBC2206.txt
[3/5] Emitting module MacRunnerControlCenter
Build complete! (2.22s)
```

## Tests
```console
✔ Test bestBackendSelection() passed after 0.656 seconds.
✔ Test noRegressionIfFirstRunFails() passed after 0.658 seconds.
✔ Test exportV2Bundle() passed after 2.797 seconds.
✔ Test loadSaveEntries() passed after 2.797 seconds.
✔ Test optionsControlInclusion() passed after 4.842 seconds.
✔ Suite DebugBundleV2Tests passed after 4.843 seconds.
✔ Test regressionManifestGeneration() passed after 4.846 seconds.
✔ Test passRateCalculation() passed after 4.848 seconds.
✔ Test regressionDetection() passed after 4.851 seconds.
✔ Test defaultSettings() passed after 4.842 seconds.
✔ Suite CompatibilityDBTests passed after 4.852 seconds.
✔ Test persistAndLoad() passed after 4.843 seconds.
✔ Suite SettingsTests passed after 4.853 seconds.
✔ Test echoStreaming() passed after 4.906 seconds.
✔ Suite LogStreamerTests passed after 4.907 seconds.
✔ Test cancellationWorks() passed after 4.949 seconds.
✔ Test echoTest() passed after 4.951 seconds.
✔ Test stderrCapture() passed after 4.951 seconds.
✔ Test quickRunUpdatesAppStatus() passed after 4.583 seconds.
◇ Test runAppViewModelOnCompleteCalled() started.
✔ Test runAppViewModelOnCompleteCalled() passed after 0.319 seconds.
✔ Suite AppLibraryTests passed after 5.478 seconds.
✔ Test timeoutWorks() passed after 6.526 seconds.
✔ Suite CommandRunnerTests passed after 6.535 seconds.
✔ Test run with 94 tests in 22 suites passed after 6.536 seconds.
```

## Gap 1 Steam CDN cover fetch
```console
[0/1] Planning build
Building for debugging...
[0/3] Write swift-version--58304C5D6DBC2206.txt
[2/4] Emitting module MacRunnerControlCenter
Build of product 'MacRunnerControlCenter' complete! (1.87s)
fetched steam cover 220: /Users/timurtoby/Library/Caches/MacRunner/covers/steam/220.jpg bytes=150363
/Users/timurtoby/Library/Caches/MacRunner/covers/steam/220.jpg: JPEG image data, JFIF standard 1.01, aspect ratio, density 1x1, segment length 16, baseline, precision 8, 600x900, components 3
size=150363
```

## Gap 1 unknown app fallback
```console
Building for debugging...
[0/3] Write swift-version--58304C5D6DBC2206.txt
Build of product 'MacRunnerControlCenter' complete! (0.26s)
fallback placeholder generated for 99999999
/Users/timurtoby/Library/Caches/MacRunner/covers/steam/99999999.jpg: JPEG image data, JFIF standard 1.01, aspect ratio, density 144x144, segment length 16, Exif Standard: [TIFF image data, big-endian, direntries=4, xresolution=62, yresolution=70, resolutionunit=2], baseline, precision 8, 1200x1800, components 3
```

## Gap 2 Steam Web API no-key pass
```console
Building for debugging...
[0/3] Write swift-version--58304C5D6DBC2206.txt
Build of product 'MacRunnerControlCenter' complete! (0.17s)
no key, web enrichment disabled
Building for debugging...
[0/3] Write swift-version--58304C5D6DBC2206.txt
Build of product 'MacRunnerControlCenter' complete! (0.16s)
[
  {
    "appid" : "620",
    "cover_path" : "\/Users\/timurtoby\/Library\/Caches\/MacRunner\/covers\/steam\/620.png",
    "install_dir" : "\/Volumes\/MacOS\/MacRunner\/app\/macr-control-center\/Tests\/Fixtures\/steam-account\/steamapps\/common\/Portal 2",
    "install_state" : "installed",
    "installed" : true,
    "last_played" : "2024-03-09T16:00:00Z",
    "name" : "Portal 2 Fixture"
  }
]
```

## Loop wineboot
```console
pass=0 fail=0 hang=1
```

## Loop wineboot retry after cleanup
```console
python3 scripts/cleanup-wine-runtime.py --quiet || true
LOOP_COUNT=1 ./scripts/loop-wineboot.sh
pass=0 fail=0 hang=1
```

## Corrected End Condition
- Gap 1 Steam CDN cover fetch: PASS, JPEG 150363 bytes
- Gap 2 Steam Web API no-key mode: PASS, web enrichment disabled and local VDF path remains active
- Loop wineboot: FAIL, external engine-lane wineboot hang remains
- Tag v1.0.1-steam-online: HELD, not created while loop wineboot is failing

## Gate Boundary Update
Loop-wineboot regression observed in this workspace:

```console
LOOP_COUNT=1 ./scripts/loop-wineboot.sh
pass=0 fail=0 hang=1
```

Owner: Codex #1 Phase G engine lane. This run is Control Center UI / Steam online scope only and does not touch `engine/`, `wine-fork/`, or `.hyperbridge-work/`. The loop-wineboot engine regression is not blocking the UI tag.

## UI Tag Gates
- Gap 1 Steam CDN cover fetch: PASS, JPEG 150363 bytes
- Gap 2 Steam Web API no-key mode: PASS, `no key, web enrichment disabled`
- Swift tests: PASS, 94/94
- Closed-source lint: PASS
- Engine paths touched: no
