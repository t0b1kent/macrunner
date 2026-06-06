# Lane Product — AI Configurator Hardening (2026-06-07)

## Status: DONE ✓

### Task 1: Merge exe_analyzer + ai/pe_analyzer — DONE
- `exe_analyzer.py` now exports `ExeAnalysis` (new unified output) + keeps `ExeMetadata` for compat
- `analyze(path)` → `ExeMetadata` (stdlib only, backwards compat unchanged)
- `analyze_full(path)` → `ExeAnalysis` (stdlib + string scan + optional lief + profile lookup)
- lief features filled if lief installed, gracefully skipped otherwise

### Task 2: Anti-cheat signature detection (note-117 kill-filter) — DONE
- `heuristics.detect_anticheat_full(raw_or_strings, imports, section_names)`
- Kernel-mode AC: EasyAntiCheat, BattlEye, Vanguard → `kernel_driver=True`
- User-mode AC: PunkBuster, VAC, XIGNCODE3, GameGuard, nProtect → `kernel_driver=False`
- **note-117 kill-filter**: `kernel_driver=True` → `verdict="unsupported: kernel driver"`
- Tests: 6 AC tests all PASS including note-117

### Task 3: Engine fingerprint — DONE
- `heuristics.detect_engine_fingerprint(imports, raw_or_strings, section_names, version_info)`
- Detects: Unity, Unreal, Godot, GameMaker, Source, RPGMaker
- Version extraction via regex for Unity (2020.3.14f1) and Unreal (5.1)
- Confidence scoring from import+string+section hit count
- Tests: 6 engine tests all PASS

### Task 4: Dynamic import scan — DONE
- `heuristics.scan_dynamic_imports(raw_or_strings)` — extracts *.dll name tokens from strings
- `analyze_full()` diffs against static imports to show only dynamic-only deps
- Also enriches `directx_version` from dynamic scan if not found statically
- Tests: 2 dynamic import tests PASS

### Task 5: Profile lookup by SHA/name — DONE
- `profiles.find_profile_by_sha(sha256)` — scans all profiles for sha256 / sha256_list field
- `profiles.find_profile_by_name(exe_stem)` — fuzzy match against id/name/filename
- `profiles.lookup_profile_for_exe(path, sha256)` — SHA first, name fallback
- Returns: {profile_id, profile_name, recommended_lane, env, anti_cheat, source}
- `analyze_full()` calls `lookup_profile_for_exe()` and populates `ExeAnalysis.profile_match`
- Tests: 3 profile lookup tests PASS

### Task 6: Tests + py_compile — DONE
- `app/configurator/tests/test_pe_analyzer.py` — 23 tests, ALL PASS
- `python3 -m py_compile` — ALL PASS

## Files changed
- `app/configurator/exe_analyzer.py` — ExeAnalysis + analyze_full() facade
- `app/configurator/ai/pe_analyzer/heuristics.py` — string scan, AC, engine FP, dynamic imports
- `app/configurator/profiles.py` — SHA/name lookup functions
- `app/configurator/tests/__init__.py` — new (empty)
- `app/configurator/tests/test_pe_analyzer.py` — new, 23 tests
- `reports/research/LANE-PRODUCT-PROGRESS.md` — this file
