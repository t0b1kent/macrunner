# REFERENCE BACKENDS FOR FAST VALIDATION

## Purpose

This document defines oracle/reference roles for HyperBridge family-fix validation.
Inner loop must stay fast: fixtures + trace checks first, app smoke later.

## Backend roles

1. **Intel SDE** (preferred semantic oracle for x86/x64 behavior)
   - Use for authoritative instruction semantics and corner-case cross-checks.
   - Best for REP/flags/partial-register edge cases.

2. **sse2neon** (SSE-to-NEON semantic mapping reference)
   - Useful for SSE2 family reasoning on ARM-oriented mappings.
   - Secondary reference, not primary oracle for full x86 machine state.

3. **FEX / Box64 / QEMU / Remill** (architectural references)
   - Useful for differential behavior checks and independent emulator comparison.
   - Good for confirming whether divergence is HyperBridge-specific.

4. **Rosetta / CrossOver** (app-level black-box references)
   - Use only for final high-level behavior checks (integration confidence).
   - Not suitable as instruction-level oracle in fast semantic loop.

## Adapter contract

All adapters should emit a normalized oracle-result payload consumed by:
- `tools/hb_oracle/compare_oracle_results.py`
- schema: `tools/hb_oracle/schema/oracle-result.schema.json`

Required top-level fields:
- `fixture_name`, `family`, `backend`
- `initial`, `final`, `expected`, `actual`
- `actual_fault`, `pass`, `diff`

## Status

- `tools/hb_oracle/sde_adapter.py` — stub interface only
- `tools/hb_oracle/hyperbridge_adapter.py` — stub interface only
- `tools/hb_oracle/expected_json_adapter.py` — stub interface only

Codex should implement real execution hooks when the target backend/tool is available.
