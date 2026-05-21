# CODEX FAST VALIDATION RULES

## Core rule

After every family-fix, Codex must run fast semantic validation first.

1. Run `tools/hb_oracle/fast_validate_family.sh <family>`
2. If **FAIL**: do **not** run app smoke; fix family semantics first.
3. If **PASS**: app smoke is allowed as next stage.

## Decision matrix

- **fast FAIL** + app not run: expected behavior; iterate on semantics.
- **fast PASS** + app FAIL: investigate boundary/Wine integration, not core family semantics first.
- **fast PASS** + app PASS: family ready to proceed.

## Required artifacts per run

- `reports/hyperbridge-validation/LATEST-FAST-VALIDATION.md`
- `reports/hyperbridge-validation/LATEST-FAST-VALIDATION.json`

Optional timing telemetry:
- `tools/build_health/measure_hyperbridge_loop.sh`
- `reports/hyperbridge-validation/BUILD-LOOP-TIMING.md`
- `reports/hyperbridge-validation/BUILD-LOOP-TIMING.json`

## Guardrails

- Keep `engine/**` read-only in tooling-only tasks.
- Prefer deterministic fixtures + FileCheck traces in inner loop.
- Run expensive Notepad++ / Wine app smoke only after fast green.
