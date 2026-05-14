# JSON Contracts

## LauncherResult
Read from `--json` output of `run-windows-app.sh`.

Key fields:
- `status`: PASS / FAIL / CRASH / TIMEOUT / MISSING_DLL
- `rc`: exit code
- `duration_ms`: runtime duration
- `arch`: PE machine type
- `d3d_enabled`, `d3d_backend`, `d3d_status`
- `d3d_trace_path`, `d3d_report_path`, `d3d_ppm_path`

## DoctorReport
Read from `reports/macr-doctor.json`.

Key sections:
- `host`: system, machine, macos
- `lanes`: arm64, x64, x86 status
- `graphics`: metal_probe_pass, render_core
- `scripts`: run_windows_app, d3d_smoke

## RealAppManifest
Read from `tests/real-app-manifests/*.json`.

Key fields per app:
- `name`, `path`, `arch`, `args`, `env`, `workdir`
- `expected_rc`, `expected_stdout_contains`
- `d3d_backend`, `allow_gui`, `timeout`
