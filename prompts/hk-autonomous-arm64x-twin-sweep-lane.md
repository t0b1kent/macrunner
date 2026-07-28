# AUTONOMOUS LANE (OFFLINE): sweep the ARM64X dual-`.data` twin class

You are running in an auto-loop. You will be relaunched in fresh threads until you write
`LOOP-STATUS: GOAL` or `LOOP-STATUS: BLOCKED` at column 0 of
`reports/research/LANE-HK-TWINSWEEP-PROGRESS.md`. Work continuously; do not wait for a human.

Repo root: `/Users/timurtoby/Documents/MacRunner/Main/MacRunner`.

## Why this lane exists — the same bug has now cost us twice

Under ARM64X, `kernelbase.dll` carries **two `.data` views** (native and EC). A static
initialiser lands in only one of them. Reached through the other view the global is **zero**, and
the first dereference is an access violation.

Two confirmed casualties:

1. `entry_sintlsymbol` — `RegSetKeyValueW` faulted on an unmirrored subkey pointer
   (kernelbase+0x59930). Recorded at `engine/wine/dlls/kernelbase/locale.c:751`.
2. `reg_mui_cache` (`registry.c:82`) — `llvm-nm` shows it at **both** `0x180150ab0` and
   `0x1801525b0`, delta `0x1b00` = exactly `MACRUNNER_HB_LOCALE_ENTRY_EC_DELTA`. Through the EC
   view the list head was NULL, so `LIST_FOR_EACH_ENTRY` faulted at kernelbase+0x5CCC8
   (`ldr w8,[x25,#0x18]`, x25==0). That killed `kernelbase`'s DllMain, which killed
   `explorer.exe`, which meant no desktop, no `winemac.drv`, **and no keyboard or mouse input for
   any guest**. Fixed 2026-07-28 in commit `e4b90849`.

Both were found one crash at a time. **Your job is to find the rest before they fire.**

## The method

`macrunner_hb_sync_locale_ec_copies` (`kernelbase/locale.c:710`) mirrors a hand-written list of
globals with `macrunner_hb_mirror_ec_copy` (a `memcpy` at a fixed delta). That list is an
enumeration somebody wrote, not a derivation — `locale.c` says so itself. So:

1. **Derive the true set.** Use `llvm-nm` (and the ARM64X load-config / CHPE metadata) on the
   shipped `kernelbase.dll` to list every symbol that appears **twice** at a fixed delta. That set
   is ground truth; the mirror list is a guess about it.
2. **Diff it against what is actually mirrored.** Anything in the first set and not the second is
   a live hazard.
3. **Rank by reachability.** A twin only bites if code reaches it through the EC view. Say which
   ones are reachable and how you know.
4. **Classify the fix per symbol — this is where a blind sweep goes wrong.** `memcpy` mirroring is
   **WRONG for any self-referential structure**: a `LIST_INIT`ed head points at itself, so copying
   the bytes leaves the twin pointing at the *native* head and the compare against the EC `&head`
   never matches. Those need `list_init()` **at the twin's own address** (see
   `reg_mui_cache_ensure_init` in `registry.c` for the shape). Critical sections, mutexes and
   anything holding a pointer back into itself are the same class. Plain scalars and tables are
   safe to `memcpy`.

Do not extend the mirror list blindly. A wrong mirror is worse than a missing one, because it
looks fixed.

## Other modules

`kernelbase` is where both casualties were, but the ARM64X twin condition is not specific to it.
Check the other x86_64-windows PEs we ship for the same double-symbol signature and say which
have it. Name what you checked and what you did not.

## Territory

- **YOURS:** `engine/wine/dlls/kernelbase/**`, `tools/**`, `reports/**`.
- **FORBIDDEN:** `engine/wine/dlls/ntdll/unix/macrunner_hb.c`, `engine/hyperbridge/**`,
  `engine/wine/dlls/winemac.drv/**`, `engine/dxmt/**` — other lanes hold uncommitted work there.
- **This is an OFFLINE lane. Never run Hollow Knight, never launch `scripts/mr-run.sh`, never
  `pkill`/`killall`.** Another lane owns the single title slot.
- **No commits, no `git add`** — the coordinator commits.

## Hard gates, each of which has cost this project a day

- **Verify the artifact, not the source.** If you build, check the SHA of what lands in
  `dist-arm64ec-spike`, not that the build exited 0. And note:
  `scripts/build-wine-arm64ec-spike.sh` does **not** build hyperbridge.
- **`not found` ≠ `not there`.** State which binary you inspected, with which tool, and how you
  distinguished a twin from two unrelated symbols that happen to share a name.
- **Quote the evidence.** An address pair and the delta, or it is a guess.
- **Getting a PID, if you ever need one:** match on `comm`, never `ps -Awwo args | grep`, which
  matches your own command line.
- **Mark every unproven statement `[HYPOTHESIS]`.**

## Reporting

- One report per iteration under `reports/phase4-hollow-knight/`, named for the question.
- Append one heartbeat line per step to `reports/research/LANE-HK-TWINSWEEP-PROGRESS.md`.

## Termination

- `LOOP-STATUS: GOAL` — the full twin set is derived from the binary, diffed against the mirror
  list, ranked by reachability, each unmirrored one classified `memcpy` vs `init-in-place`, and
  the fixes for the reachable ones are in the tree.
- `LOOP-STATUS: BLOCKED` — you need something only the operator can decide. One sentence.
- Otherwise keep going.
