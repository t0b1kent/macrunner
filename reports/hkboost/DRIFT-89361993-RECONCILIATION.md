# Build-repro drift reconciliation — HK 2x SINGLE_LOOKUP floor `89361993`

**Date:** 2026-07-04. **Investigator:** hkboost lane (Opus). **Method:** read-only symbol/mtime
forensics on main's existing artifacts (no rebuild required). **Rails honored:** main tree not
modified; only read + one scratchpad compile of main's clean-HEAD source.

## Verdict (the gate answer)

**The deployed "floor" binary `89361993` is NOT clean HEAD (e5ab613). It is `e5ab613 + the
uncommitted region-fusion WIP in `stash@{0}` (`laneA-region-fusion-wip-launchkill`)`.** A clean HEAD
rebuild is *correctly* clean and therefore differs — it lacks the region-fusion code that is baked
into the deployed binary. This is the fc1a6924-class drift, 2nd occurrence: **a deployed binary
built while an uncommitted WIP was applied, then the WIP was stashed, leaving the deploy
unreproducible from HEAD.**

Exact source delta (clean HEAD → 89361993) = `stash@{0}`, **568 insertions / 6 files**:
```
engine/hyperbridge/include/hb_codegen.h      +6
engine/hyperbridge/include/hb_context.h      +3
engine/hyperbridge/include/hb_runtime.h     +17
engine/hyperbridge/src/hb_arm64_codegen.c  +221
engine/hyperbridge/src/hb_runtime.c        +313
engine/wine/dlls/ntdll/unix/macrunner_hb.c  +18
```

## Proof chain (symbol-level, decisive)

1. **Working binary carries region-fusion code.** `nm` of the deployed floor
   `dist-arm64ec-spike/lib/wine/aarch64-unix/ntdll.so` (sha `893619931134d48a…`, the only known-good)
   contains 7 region-fusion symbols that have no business being in a "clean SINGLE_LOOKUP" build:
   `_macrunner_hb_pc_allows_region_fusion`, `_hb_arm64_codegen_region_superblock`,
   `_block_cache_region_meta`, `_trace_region_fusion_install`, `_compare_u64_region_member`,
   `_runtime_region_fusion_enabled.cached`, `_trace_region_fusion_enabled.cached`.
2. **Clean HEAD has none of them.** `git grep` for those symbols in HEAD source = empty; and a fresh
   compile of main's current (git-clean, == HEAD) `macrunner_hb.c` with the exact
   `compile_commands.json` flags produces an object **without** `_macrunner_hb_pc_allows_region_fusion`
   (2928 syms, __TEXT 431748) whereas main's build-dir `macrunner_hb.o` **has** it (2932 syms,
   __TEXT 432191, +443 bytes) — the only functional delta between the two objects is exactly the
   region-fusion additions (`pc_allows_region_fusion` + 3 string literals).
3. **The symbols are defined only in `stash@{0}`.** `git stash show -p stash@{0}` adds precisely
   `macrunner_hb_pc_allows_region_fusion`, `runtime_region_fusion_enabled`, `emit_region_*`,
   `codegen_region_superblock`, `region_meta/label/member`, etc.
4. **mtime corroboration.** In main's build dir, `macrunner_hb.o` = 22:44 (built while WIP applied),
   `libhyperbridge.a` = 23:52, deployed working `ntdll.so` = 23:54, then `macrunner_hb.c` source
   touched 23:58 (git-clean content = the stash rewrote it to HEAD on `git stash push`). Only
   `macrunner_hb.o` was stale (source newer than object) — the classic tell.

## Why clean HEAD gives rc=137 (hypothesis, needs one run to confirm)

Two candidates, not yet distinguished (both need a clean-HEAD build+run on a quiet host):
- **(likely) region-fusion is load-bearing:** the committed dispatch fast-path (`e8f7503` +
  `81669cf` default-on SINGLE_LOOKUP) has a latent init bug that the region-fusion WIP's
  `hb_runtime.c`/`hb_arm64_codegen.c` changes (+534 lines there) incidentally fix; strip them and
  ntdll faults at load (rc=137 before HB markers).
- **(less likely) the coordinator's rebuild had its own stale-.o / env issue** — rc=137 is a build
  artifact, not a HEAD source property.

## Consequence for the perf claim

**The 2.02x dispatch / 2.22x-to-swapchain SINGLE_LOOKUP number is confounded** — it was measured on a
binary that *also* contains region-fusion. It is not attributable to SINGLE_LOOKUP alone until
re-measured on a reproducible base. This is exactly why the gate must clear before any perf work.

## Recommended path to a reproducible floor (resolves Task 1 → unblocks Task 2)

1. **Commit the region-fusion WIP** (`git stash apply stash@{0}`, review, commit) so the floor's
   actual source is in HEAD, OR **discard it** and rebuild clean HEAD.
2. **Clean force-rebuild** from the committed tree (`rm -rf build-arm64ec-spike` or force-clean the
   affected objects — never trust the incremental) and re-measure. If clean-HEAD-without-fusion is
   rc=137, region-fusion is load-bearing → it must be committed, and the SINGLE_LOOKUP number
   re-taken with it in (or the init bug fixed).
3. Only then re-run the A/B; the "2x" must be reproduced from a `git status`-clean, force-clean build.

## Anti-drift procedure (make this the 3rd-time-never checklist)

Before capturing ANY milestone/floor binary as "the win":
1. **`git stash list` must be empty** (or every entry verified NOT applied to the working tree).
   The `-launchkill` stash proves a WIP was live when a build happened.
2. **`git status --porcelain` clean** for all `engine/**` source before the build that produces the
   deploy. An applied-then-stashed WIP leaves a git-clean tree but stale objects — so also:
3. **Stale-object scan:** no build object may be older than its source
   (`find build -name '*.o'` vs source mtime); force-clean any hit. One-liner in
   `scripts/hkboost-driftcheck.sh` (this lane).
4. **Symbol audit of the deploy:** `nm <deployed ntdll.so>` must contain NO symbol absent from a
   fresh clean-HEAD compile. A 30-second `nm | grep` for known-WIP tokens (here: `region_fusion`)
   catches contamination the git tree hides.
5. **The deploy must come from a force-clean build**, never an incremental one, when it will be
   floored as a reference.

## Artifacts
- Working floor: `MacRunner/artifacts/milestone-dist/hk-2x-singlelookup-89361993-UNREPRODUCIBLE-20260704-034617/`
- Scratch compile + object diffs: session scratchpad (`macrunner_hb_freshHEAD.o`, `nmA/nmB.txt`).
- Drift-check helper: `scripts/hkboost-driftcheck.sh`.
