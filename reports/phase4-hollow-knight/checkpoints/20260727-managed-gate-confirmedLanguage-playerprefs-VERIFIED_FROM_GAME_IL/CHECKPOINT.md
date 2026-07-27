# What Hollow Knight is waiting for — read from the game's own IL (2026-07-27)

**Class:** `VERIFIED_FROM_GAME_IL`. The gate is identified with method and field tokens taken
from the shipped assembly, not inferred from log markers. **Nothing is fixed.**

## The gate

```
StartManager.<Start>d__25.MoveNext        method token 0x060041d2, poll at IL_00da
    while (!confirmedLanguage) yield return null;
StartManager.confirmedLanguage            field token 0x04000DE2
```

It is a **poll predicate that never becomes true**, not a blocking wait. That is why the
thread looks alive, stays on-CPU and never advances — a week of "where is it hanging" was
looking for the wrong shape.

The only thing that sets the field:

```
public void StartManager::ConfirmLanguage()   method token 0x06000e6c, RVA 0x50d70
```

with **zero IL callers** — reachable only from a Unity UI click. `SetLanguage("EN")` is
optional; `ConfirmLanguage` alone releases the gate.

## Why the menu is on screen at all — and this part is an engine bug

`PlayerPrefs.GetInt("GameLangSet", 0)` returns **0 although the value is present in the
prefix registry**, and `PlayerPrefs.HasKey("M2H_lastLanguage")` misses too. Two independent
managed reads fail on registry content that the Windows oracle reads correctly on the same
game build — hence `Loaded system language` in run6/8/10/12 versus the oracle's
`Loaded saved language code 'EN'`.

**Therefore the saved-vs-system branch was never the switch.** run15 took the system branch
and stalled identically at the same wall, which the branch hypothesis could not explain.
The real defect sits one layer below the game: **our native PlayerPrefs → registry read**.

Full chain: PlayerPrefs cannot read the saved language → the game believes no language was
ever chosen → it shows the language menu → it parks on `while (!confirmedLanguage)` → it
waits for a click that nothing can deliver.

## Contents

- `reports/` — Q1 (the wait path), Q2 (the language-gate map), Q3 (the menu contradiction),
  Q4 (exact signatures to satisfy the wait).
- `il/` — 27 IL dumps of the relevant methods, so every claim above can be re-checked against
  bytecode rather than taken on trust.
- `journals/`, and the lane prompt that produced this.

## Two paths, not alternatives

1. **Immediate unblock:** `mono_runtime_invoke` on token `0x06000e6c` via the existing Mono
   profiler actuator (`tools/hk_language_observer/`). Releases the gate and reveals the next
   wall. Artifact discipline applies: three builds of `mono-profiler-hk_language.dll` exist,
   exclude the milestone-trace `7d403703`, verify by SHA what lands in `system32`.
2. **Real fix:** make the native PlayerPrefs registry read work. Then the game reads its
   saved language, never shows the menu, and boots the way the oracle does.

## Integrity

`SHA256SUMS` covers every file except itself; credential sweep clean.
