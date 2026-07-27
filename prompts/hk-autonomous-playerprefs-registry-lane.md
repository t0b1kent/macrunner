# AUTONOMOUS LANE: the guest cannot read registry values that are demonstrably there

You are running in an auto-loop. You will be relaunched in fresh threads until you write
`LOOP-STATUS: GOAL` or `LOOP-STATUS: BLOCKED` at column 0 of
`reports/research/LANE-HK-REGISTRY-PROGRESS.md`. Work continuously; do not wait for a human.

Repo root: `/Users/timurtoby/Documents/MacRunner/Main/MacRunner`.

## The bug, already narrowed for you

Hollow Knight shows its language-select menu and parks forever, because
`UnityEngine.PlayerPrefs.GetInt("GameLangSet", 0)` returns **0** and
`PlayerPrefs.HasKey("M2H_lastLanguage")` **misses** — while those values are present in the
prefix registry and the Windows oracle reads them correctly on the same game build.

This is **not** a content problem and **not** a copy problem. I verified both:

- Template `artifacts/hk-windows-oracle-prefix-template-20260721-225722-v3/user.reg` has
  section `[Software\\Team Cherry\\Hollow Knight]` (line 640, 40 values) containing
  `"GameLangSet_h1172976845"=dword:00000001` and
  `"M2H_lastLanguage_h3859156181"=hex:45,4e,00`.
- A live run prefix (`artifacts/_mr-run-aa-hk-mono-jit-hash-cycle-causal-20260726-1042/user.reg`)
  contains the same section and both values.

So the bytes are on disk in the right place under the right key, and the guest still reads
zero. **The defect is in the read path.** That is your subject.

Why it matters: the managed lane decompiled the game and proved the menu predicate reduces,
on desktop, to exactly this read — `menu ⟺ !CheckIsLanguageSet() && Platform.ShowLanguageSelect`,
and `DesktopPlatform.ShowLanguageSelect` is a constant `true` (token 0x06000f82). **When the
read works, the boot skips the `confirmedLanguage` poll entirely.** Fixing this removes the
gate rather than working around it.

## Do NOT debug this with 40-minute Hollow Knight runs

The whole point of this lane is that the question is testable in seconds. Write a **tiny
x86-64 Windows probe** that does `RegOpenKeyEx(HKEY_CURRENT_USER, "Software\\Team Cherry\\Hollow Knight")`
+ `RegQueryValueEx` for both value names, prints what it got, and exits. Run that under our
stack against a prefix seeded with the same `user.reg`. That is your instrument; iterate on
it, not on the game.

Better still, start offline: read the code path first and see whether the bug is visible by
inspection before spending any Wine time at all.

## Hypotheses worth checking, none confirmed

- **Architecture/WOW64 redirection.** HK is x64 under our translation. If anything in our
  stack treats the process as 32-bit, `Software\...` reads can be redirected to a
  `WOW6432Node` path that does not exist — which produces exactly "value is there, read
  returns nothing". Check what `KEY_WOW64_*` / redirection logic our
  `advapi32/registry.c` and `ntdll/unix/registry.c` apply, and what the game's actual
  `RegOpenKeyEx` flags are.
- **Value type.** Unity stores strings as `REG_BINARY` (`hex:45,4e,00` = `"EN\0"`) and ints
  as `REG_DWORD`. A read path that mishandles `REG_BINARY`, or that requires an exact type
  match and gets it wrong, would fail `HasKey` too — and `HasKey` failing is a strong clue,
  because it does not care about the value at all.
- **Load timing.** Does `user.reg` reach the *running* wineserver's registry, or only the
  file on disk? If the prefix is seeded after wineserver has started, the on-disk file can be
  correct while the live registry is empty. Check when the template is copied relative to
  wineserver start, and whether anything reloads the hive.
- **Key path handling.** The section name contains a space (`Team Cherry`). Check case
  handling and separator handling end to end.

Prove which one it is. Do not fix all four blind.

## Territory — three other agents are in this repo

- **YOURS:** `engine/wine/dlls/advapi32/**`, `engine/wine/dlls/ntdll/unix/registry.c`,
  `engine/wine/server/registry.c` if it exists, plus `tools/**` and `reports/**`.
- **FORBIDDEN — the engine lane is editing these right now:**
  `engine/wine/dlls/ntdll/unix/macrunner_hb.c`, `engine/hyperbridge/**`,
  `engine/wine/dlls/winemac.drv/**`, `engine/dxmt/**`. Two uncommitted engine patches are
  live in the tree; do not touch or revert anything outside your files.
- **NEVER run Hollow Knight and never launch `scripts/mr-run.sh`.** Another lane owns the
  single JIT title slot and has runs in flight.
- **Your probe may run** — it is seconds, not a title — but only if: no HK run is live
  (check `ps` for `mr-run.sh` and for `extracted-hollow-knight` first), it uses its own
  disposable prefix, and it never touches
  `artifacts/hk-windows-oracle-prefix-template*`. Never `pkill`/`killall`.
- **No commits, no `git add`.** Preserve work in a checkpoint if it is substantial.

## Hard gates — each has cost this project a day

- **Prove the tool before trusting the number.** Your probe must first read a value you
  planted yourself and print it correctly. A probe that has never returned a known-good value
  cannot be used to prove absence.
- **`not found` ≠ `not there`.** Say which key, which flags, which type, and which API.
- **Verify the artifact actually landed** — after any build, check the SHA of the file that
  ends up in the dist/prefix, not that the build exited 0. We have shipped stale binaries
  while believing they were fresh; the root cause was a `makedep.c` dependency bug.
- **Fix the family, not the sample.** If you find the defect at one call site, check whether
  the same mistake exists across the read path before declaring it fixed.
- **Mark every unproven statement `[HYPOTHESIS]`.**

## Reporting

- One report per iteration under `reports/phase4-hollow-knight/`, named for the question.
- Append one heartbeat line per step to `reports/research/LANE-HK-REGISTRY-PROGRESS.md`;
  writing a report counts as work.
- State evidence: the exact API call, flags, returned status, and bytes.

## Termination

- `LOOP-STATUS: GOAL` — the guest reads `GameLangSet` as 1 and `M2H_lastLanguage` as `"EN"`
  through the same path the game uses, proven by your probe, with the defect named and the
  fix in the tree.
- `LOOP-STATUS: BLOCKED` — you need something only the operator can decide (a title run to
  confirm end-to-end, a destructive action, quota). Name it in one sentence.
- Otherwise keep going.
