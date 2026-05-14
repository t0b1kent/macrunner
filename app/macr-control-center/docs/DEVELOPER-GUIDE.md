# Developer Guide

## Build
```bash
cd app/macr-control-center
swift build
```

## Run
```bash
./scripts/run-control-center.sh
```

## Test
```bash
swift test
```

All 74 tests pass deterministically in ~6 seconds.

## CI Verification
```bash
./scripts/ci-verify.sh
```

Runs build, tests, structural checks, and generates `reports/ci-verification.json`.

## Project Layout
- `Models/` — Codable structs for JSON contracts (MacRunnerTask, AppEntry, LauncherResult, etc.)
- `Services/` — ConfigStore, CommandRunner, TaskQueue, PPMParser, PathSafety, DebugBundleExporter, DebugBundleExporterV2, FailureClassifier, LogStreamer, PEInspectionService, RunHistoryStore, CompatibilityStore, ReportReader, CodexPromptService, RegressionManifestService
- `Views/` — SwiftUI views (ContentView, AppLibraryView, TaskQueueView, LiveLogView, RunPanelView, D3DArtifactsView, DoctorView, BottleManagerView, WineProcessView, CorpusView, PerformanceView, CompatibilityDBView, DebugBundleView, AppPackagingView, OnboardingView, AppImportWizardView, AppDetailView, FailureTriageView, InlineD3DArtifactsView, IntegrationBlocksView, ReleaseManagerView, LocalTrialWizardView, SettingsView)
- `ViewModels/` — ObservableObjects bridging UI and services
- `Fixtures/` — Mock JSON reports for development without Wine

## Adding a New View
1. Add View in `Views/`
2. Add ViewModel in `ViewModels/`
3. Register tab in `ContentView.swift`
4. Add tests in `Tests/`

## Features

### Task Queue / Job System
- Serial execution queue with cancel/retry
- Persistent run history (last 200 entries)
- Enqueue Doctor, Verify, Cleanup, D3D Smoke, and custom commands

### Compatibility Lab
- Filter by status, arch, D3D backend, category
- Edit compatibility entries inline
- Export/import JSON
- Regression detection and manifest generation
- Pass rate and best backend tracking

### App Import Wizard
- PE deep inspection (arch, subsystem, D3D detection, DLL dependencies, imports, sections)
- Compatibility DB cross-check on import
- Step-by-step wizard with drag-and-drop

### Smart Failure Triage
- Automatic classification with confidence scores
- Compatibility DB history awareness
- Actionable recommendations and quick actions
- Inline D3D artifact viewer for non-PASS runs

### D3D Artifact Workbench
- PPM viewer, trace viewer with filtering, IR/JSON report viewer
- Side-by-side comparison mode
- Direct D3D smoke test enqueue

### Bottle Manager
- Size calculation, stale detection, batch delete
- Repair via wineboot -u
- Archive with ditto

### Process Monitor
- Auto-refresh every 3 seconds
- Search/filter, kill from context menu
- Copy PID to clipboard

### Doctor/Verify Deep Dashboard
- Health score badge with color-coded severity
- Detailed lane, graphics, tools, and scripts diagnostics
- Actionable recommendations based on failures
- Export combined doctor/verify JSON report

### Performance Lab
- Time range filter (24h/7d/30d/all)
- Backend breakdown with pass/fail bars
- Duration histogram
- CSV and JSON export

### Debug Bundle v3
- Granular options (artifacts, logs, reports, system info, git diff)
- Path redaction (home dir ~, MACRUNNER_ROOT)
- Max file size truncation with head capture
- Recent bundles list
- Reveal in Finder

### .app Packaging
- Package any app in the library as a .app bundle
- Configurable bundle name and output path
- Generates launcher script and Info.plist

### Release Manager
- Export versioned release bundles (zip)
- Include compatibility DB, performance reports, manifests, debug bundles
- Add release notes markdown
- Browse and manage previous releases

### Local Trial Wizard
- Step-by-step wizard for first-time app runs
- Select executable, configure D3D/backend, run, review results, save to library
- Integrated failure triage and D3D artifact preview
- Automatic compatibility DB update on save

### Integration Blocks
- Run integration block suite (`run-integration-blocks.sh`)
- JSON result parsing with pass/fail icons
- Duration tracking per block

### Codex Prompt Generator
- Generate fix prompts from LauncherResult + FailureClassifier diagnosis
- Generate corpus analysis prompts for regression patterns

## Testing Architecture
- Swift Testing with `@MainActor` for view model tests
- Deterministic test runs (no flaky tests)
- Model tests for all new Codable types
- Service tests for store operations
- Max 200 entries in RunHistoryStore to prevent bloat
