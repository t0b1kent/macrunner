# MacRunner Control Center

Native macOS GUI that manages MacRunner core runtime through stable CLI/scripts/reports without touching the core.

## Architecture

- SwiftUI app with TabView navigation
- Calls existing scripts as subprocesses
- Reads JSON reports and artifacts
- Stores local config in `~/Library/Application Support/MacRunnerControlCenter/`
- Never modifies Wine, D3D bridge, or engine code

## Tabs

| Tab | Purpose |
|-----|---------|
| Apps | App library with import, run, edit, and detail views |
| Run | One-click run panel for any .exe with D3D/backend options |
| Queue | Serial task queue with cancel/retry/clear |
| Logs | Live log streamer with search, auto-scroll, and export |
| D3D | D3D artifact workbench: PPM, trace, IR, report, compare mode |
| Doctor | Deep dashboard: health score, lane/tools/graphics diagnostics, recommendations |
| Bottles | Bottle manager: stale detection, repair, archive, batch delete |
| Processes | Live Wine/process monitor with search and kill |
| Corpus | Real app corpus manager: batch run, manifest editor, compatibility sync |
| Perf | Performance lab: time range filters, backend breakdown, histogram, CSV/JSON export |
| Blocks | Integration blocks UI: run blocks and display pass/fail results |
| Compat | Compatibility DB v2: regression detection, pass rate, best backend, manifest export |
| Bundle | Debug Bundle v3: granular options, path redaction, max file size, recent bundles |
| Package | Package any app as a native .app bundle |
| Releases | Release manager: export versioned zip bundles with DB, reports, manifests, notes |
| Trial | Local trial wizard: 5-step guided first run with triage and save |
| Settings | App settings: root path, timeouts, defaults, debug mode |
