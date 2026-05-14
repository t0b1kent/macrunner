# MacRunner Control Center

Native macOS GUI for managing MacRunner without touching the core runtime.

## Scope

- **Allowed**: SwiftUI app, CLI bridge, JSON report reading, app manifests, bottle management, process monitoring, debug bundle export.
- **Forbidden**: Never rewrites Wine/runtime, D3D bridge, engine/graphics behavior, `run-windows-app.sh` semantics, or architecture routing.

## Build

```bash
cd /Users/timurtoby/Developer/MacRunner/app/macr-control-center
swift build
swift test
```

## Run

```bash
./scripts/run-control-center.sh
```

## Package

```bash
./scripts/package-control-center.sh
```

Output: `dist/MacRunnerControlCenter/`

## CI Verification

```bash
./scripts/ci-verify.sh
```

Runs build, tests, structural checks, and generates `reports/ci-verification.json`.
