# ROOT CAUSE: PlayerPrefs reads fail because the HyperBridge import-semantic stub fakes all advapi32 W-registry calls

**Date:** 2026-07-27 · **Lane:** HK-REGISTRY · **Status:** DEFECT NAMED + PROVEN with a 30-second probe. Fix is a one-line removal in engine-lane territory (`macrunner_hb.c`), prepared as a ready patch below.

## TL;DR

`macrunner_hb_try_registry_semantic` (`engine/wine/dlls/ntdll/unix/macrunner_hb.c:30794`, wired into
the import-thunk semantic chain at `:32604`) **short-circuits every advapi32 W-registry import**
from translated x64 guests:

- `RegOpenKeyExW` / `RegOpenKeyW` / `RegCreateKeyExW` → write a **fabricated sequential handle**
  (`0x6f00f0000000`, `+1` per call) and return `ERROR_SUCCESS`. The real registry is never touched.
- `RegQueryValueExW` → writes `type=REG_SZ, size=0`, returns **`ERROR_FILE_NOT_FOUND`** — always.
- `RegSetValueExW` / `RegDeleteValueW` / `RegCloseKey` → pretend `ERROR_SUCCESS`.

Hollow Knight's PlayerPrefs opens `HKCU\Software\Team Cherry\Hollow Knight` with
**RegOpenKeyExW** (verified by disassembly of the call site at `UnityPlayer.dll+0x7cb4d4`:
`rcx=0x80000001`(HKCU), `r8=0`, `r9d=0x20019`(KEY_READ), no `KEY_WOW64_*` flags) and queries values
with **RegQueryValueExA** (IAT call sites in the PlayerPrefs region). So the game gets a fabricated
handle (open "succeeds"), and every query on it fails with `ERROR_INVALID_HANDLE` (6) —
`GetInt("GameLangSet")` → default 0, `HasKey("M2H_lastLanguage")` → false. Exactly the observed
language-menu park. The Wine registry stack itself is fully functional (proven below).

## Probe evidence (`tools/hk_registry_probe/`, run dir `reports/phase4-hollow-knight/registry-probe-20260727-204332` + re-runs)

Probe = tiny x86-64 exe, disposable prefix seeded with the oracle template `user.reg`
(SHA `2bc52f0a…40c9a3`), run under `engine/wine/dist-arm64ec-spike` (the exact dist the HK lane
uses; artifact SHAs recorded in `dist-artifacts.sha256`). Control gate first: probe creates its own
key, writes+reads a DWORD and a REG_BINARY — **PASS**, so the tool is proven.

| Call | Result | Meaning |
|---|---|---|
| `RegOpenKeyExW(HKCU, "Software\Team Cherry\Hollow Knight", 0, KEY_READ)` | `0` (SUCCESS), **hkey=`0x6f00f0000000`** | fabricated handle from the stub |
| `RegQueryValueExW "GameLangSet_h1172976845"` on it | `2` (ERROR_FILE_NOT_FOUND) | the stub's hardcoded answer |
| `RegQueryValueExA` same name, same handle | `6` (ERROR_INVALID_HANDLE) | real API + fake handle |
| `RegEnumValueA` on the fake handle | `6` at index 0 | fake handle again |
| `RegSetValueExA` on a second fake handle (`0x6f00f0000001`) | `6` | writes die too |
| **`RegOpenKeyExA` same path** | `0`, **hkey=`0x3c`** (real) | A-variant is NOT in the stub list |
| `RegQueryValueExA GameLangSet` via A-open | `0`, type=4, **val=1 MATCH** | **GOAL value read** |
| `RegQueryValueExA M2H_lastLanguage` via A-open | `0`, type=3, **`45 4e 00` = "EN\0" MATCH** | **GOAL value read** |
| `NtOpenKeyEx` direct (HKCU → Software → Team Cherry\Hollow Knight) | `0`, real handles `0x40/0x44/0x48` | ntdll path clean |
| `NtQueryValueKey GameLangSet` | `0`, type=4, **dword=1** | server has the value |

Also: `IsWow64Process` → **0** (the WOW64-redirection hypothesis is dead — `wow_peb` is NULL for an
x64 main image, `kernelbase/open_key` takes the direct `NtOpenKeyEx` path, and server-side
`KEY_WOWSHARE` only covers `HKLM\Software\Classes`). Load timing is dead too — the running
wineserver provably holds the values (the direct-ntdll read returns them), and `pref-state-watch-run12`
snapshots show the section surviving repeated in-run wineserver saves.

## Hypotheses verdict (all four from the brief)

1. **WOW64 redirection — DISPROVEN.** x64 guest, `is_wow64=0`, game passes no `KEY_WOW64_*` flags
   (disassembly), probe opens with `KEY_READ` and both WOW64 flag variants hit the same stub.
2. **Value type — DISPROVEN.** REG_DWORD and REG_BINARY both read fine through the real path
   (probe: `type=4 val=1`, `type=3 "EN\0"`).
3. **Load timing — DISPROVEN.** The live wineserver holds the values; direct ntdll reads succeed
   against the same server the game would use. (`user.reg` on disk == in-memory image.)
4. **Key path handling — DISPROVEN.** "Team Cherry" with a space opens fine via A/ntdll paths.

**Actual root cause: the HyperBridge import-semantic registry stub** (above). Introduced in
`2728b13b` (2026-06-01, "checkpoint(Lane A) + GATE verdict…") as part of a semantic-shim batch and
active ever since. The current uncommitted engine patch does not touch it.

## Consistency check with the run12 session-count increment

`pref-state-watch-run12` shows `unity.player_session_count` going `1→2` mid-run — writes *do*
land. Consistent because the stub list covers only `RegCreateKeyExW`, not **`RegCreateKeyW`**
(present in UnityPlayer's IAT, hint 625) and none of the ANSI set/query variants
(`RegSetValueExA` is real). `[HYPOTHESIS]` Unity's startup session bookkeeping uses the
non-stubbed create/write entry points while its PlayerPrefs reads use the stubbed `RegOpenKeyExW`.

## The fix (engine-lane territory — patch prepared, not applied by this lane)

Remove the registry semantic from the import chain so W calls fall through to
`macrunner_hb_call_arm64_pe_import12_for_ctx` — the real ARM64 advapi32 implementation that the
A-variant already takes successfully (this is what produced `val=1` / `"EN"` in the probe).
Ready patch: `reports/phase4-hollow-knight/REGISTRY-SEMANTIC-STUB-REMOVAL-20260727.patch`.
Family note: the whole stub (all 7 APIs) must go, not just `RegOpenKeyExW`; the same "fake success"
pattern in `macrunner_hb_try_security_token_semantic` (`OpenProcessToken`/`OpenThreadToken`) is a
same-class risk for other subsystems — separate audit, flagged only.

## Verification after the fix lands (30 s)

```bash
./tools/hk_registry_probe/run-probe.sh A   # refuses to run while an HK title run is live
```
Expect: `OPEN RegOpenKeyExW ... -> 0 (hkey=0x<small real handle>)`, both queries `MATCH`,
`RESULT: HK VALUES READ OK`, `rc=0`. Probe refuses to run while `mr-run.sh`/HK is live and only
ever uses its own disposable prefix (`artifacts/_hk-reg-probe/`).

## Why this closes the language gate (from the managed lane)

`menu ⟺ !CheckIsLanguageSet() && ShowLanguageSelect(true)`; `CheckIsLanguageSet()` reduces to
`PlayerPrefs.GetInt("GameLangSet",0) > 0`. With the stub removed, that read returns 1 from the
seeded prefix, the menu branch is skipped, and the boot never parks on `confirmedLanguage`.
