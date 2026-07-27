# Registry fix verified end-to-end — the language gate is gone (2026-07-28)

**Class:** `VERIFIED_END_TO_END`. Hollow Knight reads its saved language from the prefix
registry, skips the language-select menu entirely, and reaches
`Performing automatic level start.` — **with the actuator deliberately OFF.**

## The run

`laneA-registryfix-verify-30-try1-042607`, actuator `MACRUNNER_HB_START_GAME_ACTUATOR=0`
on purpose (with it on, it would press `ConfirmLanguage` itself and the run would prove
nothing), and the seven run25/27/29 experiment probes disabled for a clean baseline.

| marker | count | meaning |
|---|---|---|
| `Loaded saved language code` | 2 | the oracle's SAVED branch (idx33) |
| `Loaded system language` | 0 | the broken fallback branch is gone |
| `Performing automatic level start.` | 1 | boot advanced past the gate |
| `Making UI menu lean.` | 0 | next wall, still open |

## Why this closes the language saga

The managed lane's decompile reduced the menu predicate on desktop to exactly one input:
`menu ⟺ PlayerPrefs GameLangSet read returns 0` (`DesktopPlatform.ShowLanguageSelect` is a
constant `true`, token 0x06000f82). Our own `macrunner_hb_try_registry_semantic` was faking
all seven advapi32 W-registry APIs since `2728b13b` (2026-06-01), so that read always returned
nothing. Removing the shim (50 lines) restores it — API-level proof came first from the
30-second probe (`RegOpenKeyExW` real handle `0x38` instead of synthetic `0x6f00f0000000`,
`RegQueryValueExW GameLangSet` rc=0 `dword=1`, `M2H_lastLanguage` `"EN"`, control PASS), and
this run is the end-to-end consequence.

## What is NOT claimed

- No gameplay. `Making UI menu lean.` never appears; the boot meets a further wall.
- This run later quiet-stalled in early boot with all 49 threads parked — a separate anomaly
  the engine lane is tracking, downstream of the gate and unrelated to this fix.
- The engine source change is applied in-tree but not committed (the file also carries the
  engine lane's in-progress work); it is reproducible from
  `REGISTRY-SEMANTIC-STUB-REMOVAL-20260727.patch`, and the tree state at verification is
  preserved here as `engine-tree-at-verification.patch`.

## Integrity

`SHA256SUMS` covers every file except itself.
