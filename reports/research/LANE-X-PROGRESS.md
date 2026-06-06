# Lane X Progress — Triage Hardening (2026-06-07)

## Status: DONE ✓

### Task 1: Kill CLASS_ATOM_MISMATCH false positive — DONE
- **Bug 1**: `"unity" in line` case-sensitive, missed `UnityWndClass` (capital U).
  Fixed: `"unity" in line.lower()`.
- **Bug 2**: `last_reg_atom != last_create_atom` cross-comparison compared atoms from unrelated
  windows in multi-window apps (Unity registers #32769, Message, OleMainThreadWndClass, etc. before
  UnityWndClass → atoms always differ). Removed both cross-comparisons; per-class dict check in the
  create_window loop is authoritative.
- **Guard added**: `if unity_create_window_success: class_atom_mismatch = False; class_instance_mismatch = False`
- **Fixture added**: `fixtures/window_unity_success_no_fp_run/` — multi-window run with successful
  UnityWndClass → now produces WINDOW_GATE_SUCCESSFUL, not CLASS_ATOM_MISMATCH.
- **Verified**: `window_atom_mismatch_run` fixture still correctly fires CLASS_ATOM_MISMATCH ✓

### Task 2: analyze_generic_fallback.py — DONE
- Created `tools/triage/analyze_generic_fallback.py` with routing table:
  - c0000026 / seh-host-boundary → GENERIC_INVALID_DISPOSITION_SEH → Lane A (0.55)
  - c000001d / illegal instruction → GENERIC_ILLEGAL_INSTRUCTION → Lane B (0.55)
  - c0000005 / access violation → GENERIC_ACCESS_VIOLATION → Lane C (0.50)
  - assertion failed → GENERIC_ASSERTION_FAILED → Lane A (0.55)
  - Vulkan/Metal/WineD3D → GENERIC_GRAPHICS_FAULT → Lane D (0.45)
  - wow64/xtajit/btcpu → GENERIC_WOW64_FAULT → Lane A (0.50)
- Wired into `classify_run.py` ANALYZERS list.
- Priority: GENERIC_ BLOCKED = 30 (below all real BLOCKED classes, above TRACE_INSUFFICIENT@20).
- GENERIC_ UNKNOWN = 15 (above bare UNKNOWN@10).

### Task 3: flight.jsonl diagnosis — DONE
- **Finding**: mr-run.sh writes to `artifacts/flight/<timestamp>-<pid>/flight.jsonl`, NOT the
  run directory. `find_logs(run_dir)` never finds it.
- **classify_run.py**: now prints clear WARNING with env var and TRIAGE-NEEDS.md pointer when
  no flight.jsonl found (instead of silent degradation).
- **TRIAGE-NEEDS.md**: written with exact harness change for Lane A — set `FLIGHT_PATH` inside
  `$MR_RUN_DIR` when a run directory exists, or copy after run completion.

### Task 4: New fixtures — DONE
- `fixtures/gfxdevice_c0000026_seh_detail_run/` — with full `-detail` line (exception=c0000026,
  ldr_status=8000001a, module=0, resume=stop-unwind). Produces GFXDEVICE_SEH_HOST_BOUNDARY_BEFORE_D3D11 / SELF_CHECK=PASS ✓
- `fixtures/pe32_wow64cpu_not_loaded_run/` — syswow64 DLLs missing, no wow64 activity.
  Produces PE32_WOW64CPU_NOT_LOADED / SELF_CHECK=PASS ✓
- `fixtures/window_unity_success_no_fp_run/` — see Task 1.

### Task 5: Validation — DONE
- `python3 -m py_compile tools/triage/*.py` → ALL PASS ✓
- `regression_tracker.py` → green, LATEST = GFXDEVICE_SEH_HOST_BOUNDARY_BEFORE_D3D11 / 0.95 ✓
- `classify_run.py` on real runs → correct OWNER/CLASS ✓

## Files changed (named, never -A)
- `tools/triage/analyze_window_gate.py` — fix unity detection + remove broken cross-comparisons
- `tools/triage/analyze_generic_fallback.py` — new (generic routing by exception code)
- `tools/triage/classify_run.py` — add generic_fallback to ANALYZERS, priority scores, flight warning
- `tools/triage/fixtures/window_unity_success_no_fp_run/run.log` — new fixture
- `tools/triage/fixtures/gfxdevice_c0000026_seh_detail_run/run.log` — new fixture
- `tools/triage/fixtures/pe32_wow64cpu_not_loaded_run/run.log` — new fixture
- `reports/research/TRIAGE-NEEDS.md` — flight.jsonl harness gap documented for Lane A
- `reports/research/LANE-X-PROGRESS.md` — this file
