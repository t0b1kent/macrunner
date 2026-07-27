# AUTONOMOUS LANE (OFFLINE, NO RUN SLOT): ask Hollow Knight's own C# what it is waiting for

You are running in an auto-loop. You will be relaunched in fresh threads until you write
`LOOP-STATUS: GOAL` or `LOOP-STATUS: BLOCKED` at column 0 of
`reports/research/LANE-HK-MANAGED-PROGRESS.md`. Work continuously; do not wait for a human.

Repo root: `/Users/timurtoby/Documents/MacRunner/Main/MacRunner`.

## Why this lane exists

For a week we have been inferring what Hollow Knight wants from the outside — from log
markers, oracle diffs and process state. **The game's own managed code is sitting right
there and nobody has read it.** Your job is to stop the guessing.

Another lane owns the engine and the single JIT run slot and is actively iterating. You are
offline: you read assemblies and write findings. You never run the game.

## Where the code is

- Live install: `../game-hollow.knight-(89718)/extracted-hollow-knight-1.5.12620/Hollow Knight_Data/Managed/`
  (note the DOT in `game-hollow.knight` — a space-separated glob silently misses it, that has
  bitten this project twice).
- Known-good copy, 3652608 bytes, if the live one is unavailable:
  `reports/phase4-hollow-knight/checkpoints/a1-unknown-forensic-20260719-NOT_GOLDEN/payload/focus-milestones-aa-runtime-results-v2/9526702e-44ee-4b34-873b-675c48ec6505/.A1.staging/arm-root/game/Hollow Knight_Data/Managed/Assembly-CSharp.dll`

No decompiler is installed (`ilspycmd`, `monodis`, `ikdasm`, `dotnet` all absent). Installing
one is fine — prefer a user-local install that does not touch the engine toolchain. If
installation is blocked, a CLI-metadata + IL reader you write yourself is acceptable; say so
and show the method bodies you decoded.

## The questions, in order

1. **What happens between oracle idx38 and idx40?** Every one of our runs stops right after
   `Unloading 5 Unused Serialized files`, and the next oracle line we never reach is
   `Didn't need to wait for PlayerPrefs load.` Find the managed code on that path. What
   does it wait on, and is it a blocking wait or a poll predicate that never becomes true?
   **Name the exact method and the native call it bottoms out in** — that is what the engine
   lane needs to instrument.
2. **What actually gates the language flow?** `GameLangSet_h1172976845`,
   `M2H_lastLanguage_h3859156181`, the `Loaded saved language code 'EN'` branch versus the
   `Loaded system language 'English'` branch, `ConfirmLanguage`, `SetLanguage`, and
   `allowSceneActivation`. Which of these is a precondition for which? A run with the SYSTEM
   branch (run15) stalled at the same idx38 wall as the saved-language runs, so the branch is
   **not** the switch — find what is.
3. **Resolve the menu contradiction.** run10 captured the LANGUAGE SELECT MENU on screen
   (`checkpoints/20260727-FIRST-VISIBLE-PIXEL-.../evidence/FIRST-VISIBLE-PIXEL-language-select.png`).
   A prefix that already carries a saved language should not be asking which language. Under
   what conditions does HK show that menu? That screenshot is hard evidence about the gate
   and it is currently unused.
4. **What would the game need to receive to proceed?** If the answer is an input event, say
   exactly which one and on which object. If it is a managed call, name the full signature —
   the project already has a Mono profiler actuator (`tools/hk_language_observer/`) that can
   invoke a method directly, so a precise signature is immediately actionable.

## Territory — another agent is editing this repo right now

- **YOURS:** `tools/**`, `reports/**`, and read-only access to the game's `Managed/` directory.
- **FORBIDDEN:** every engine path — `engine/wine/**`, `engine/hyperbridge/**`,
  `engine/dxmt/**`. Two uncommitted engine patches are live in the tree
  (winemac toplevel fallback, SMC reverify); do not touch or revert anything there.
- **NEVER run Hollow Knight, never launch `scripts/mr-run.sh`, never `pkill`/`killall`.**
  Another lane owns the run slot and has runs in flight.
- **Never modify the game install** — read only. And never delete
  `artifacts/hk-windows-oracle-prefix-template*`.
- **No commits, no `git add`.**

## Established — do not re-derive

- The black frame is solved and was **not** graphics: the Metal view was never parented
  (`GA_ROOT(hwnd)=0` → `client_surface_update` early-return); a 19-line winemac fix put the
  language menu on screen. Graphics is exonerated layer by layer.
- Mono's self-modifying code ran against stale translations because the icache-flush callback
  was a no-op stub; that is fixed and proven live (3295 evictions), and the remaining stall is
  **not** stale translation.
- 25 historical runs DO reach `Performing automatic level start.`; all 25 lack
  `Making UI menu lean.`
- Base-rate warning from the engine lane: `laneA-run-hk.sh` runs reach level start about 1.2%
  of the time, so small-sample "regressions" on that runner are usually noise.

## Hard gates

- **Verify what you decoded.** Quote the IL or the reconstructed C#. A claim about control
  flow with no method body behind it is a guess, and guesses are what this lane exists to
  replace.
- **`not found` ≠ `not there`.** If a symbol is missing, say which assembly you searched and
  how. HK code may live in `Assembly-CSharp.dll`, `Assembly-CSharp-firstpass.dll`,
  `UnityEngine.CoreModule.dll` or the `PlayMaker` assemblies.
- **Mark every unproven statement `[HYPOTHESIS]`.**
- Do not speculate about our engine. Your subject is the game.

## Reporting

- One report per iteration under `reports/phase4-hollow-knight/`, named for the question.
- Append one heartbeat line per step to `reports/research/LANE-HK-MANAGED-PROGRESS.md`;
  writing a report counts as work too.
- State evidence: method names, signatures, decompiled snippets, assembly and token.

## Termination

- `LOOP-STATUS: GOAL` — you can name, with code behind it, what Hollow Knight is waiting for
  at the idx38 wall and what would satisfy it.
- `LOOP-STATUS: BLOCKED` — you need something only the operator can decide (a tool install
  that needs sign-off, quota). Name it in one sentence.
- Otherwise keep going.
