# AUTONOMOUS LANE: guest input never reaches the game — same broken window hierarchy as the black frame

Auto-loop lane. Relaunched in fresh threads until you write `LOOP-STATUS: GOAL` or
`LOOP-STATUS: BLOCKED` at column 0 of `reports/research/LANE-HK-INPUT-PROGRESS.md`.
Repo root: `/Users/timurtoby/Documents/MacRunner/Main/MacRunner`.

## The finding, measured by the operator today — this is not a hypothesis

Hollow Knight displayed its language-select menu on screen and the operator tried **both
keyboard and mouse**. Nothing responded. The game was demonstrably ALIVE while he did it:

- game process at **110.7% CPU** — it is spinning, not hung;
- `Loaded system language` x2 — the language stage was reached;
- the menu was rendered on screen (screenshot captured);
- the 110% is exactly the `while (!confirmedLanguage) yield return null;` poll that the
  managed lane decompiled (`StartManager.<Start>d__25.MoveNext`, token 0x060041d2,
  field 0x04000DE2) — a per-frame poll, silent by design, so a flat log proves nothing.

So the game renders, polls, and waits for a click that never arrives. **Input is a real,
now-proven blocker**, not a suspicion. Until it is fixed the title is unplayable even once it
boots, and every workaround we build (actuators, registry seeding) is routing around it.

## Leading hypothesis, and it is a strong one

Yesterday we proved this window has **`GA_ROOT(hwnd) = 0`**: at swapchain time
`macdrv_client_surface_create` saw a null root, so `client_surface_update` early-returned and
the Metal view was **never parented** — that was the black frame, fixed with a toplevel
fallback in `macdrv_client_surface_update` (19 lines).

The input path has the same shape. `engine/wine/dlls/winemac.drv/window.c:812`:

```c
if (!(hwnd = NtUserGetAncestor(hwnd, GA_ROOT))) return;
```

An early return on a null `GA_ROOT`, in the event path this time. `window.c:1344` also routes
`WM_MOUSEACTIVATE` through `NtUserGetAncestor(hwnd, GA_ROOT)`, and `window.c:319`/`:773` use
it too. **[HYPOTHESIS] one broken window hierarchy, two symptoms: no picture (fixed) and no
input (not fixed) — yesterday's fix patched the rendering path and left the event path.**

Prove or refute it. If true, the fix is likely the same shape as the one that already worked.

## Where to look

- `engine/wine/dlls/winemac.drv/event.c` — `macdrv_key_event` dispatch at `:555`,
  `macdrv_mouse_button` at `:572`.
- `engine/wine/dlls/winemac.drv/keyboard.c:967` — `macdrv_key_event`.
- `engine/wine/dlls/winemac.drv/window.c` — the `GA_ROOT` sites above, and the toplevel
  fallback already added for the surface path; read it, it is your model.
- Focus matters too: a Cocoa window that never becomes key gets no keystrokes at all. Check
  whether the window ever becomes key/main, not just whether it is visible.

## Hard gates — each cost this project a day

- **Instrument before concluding.** Zero input-event lines in the current logs proves nothing:
  the driver may not log them at all. `not logged` != `did not happen`. Add a counted trace on
  the event path and prove it fires for a control (a synthetic event, or another window) before
  trusting its silence for the game.
- **Prove the tool before trusting the number** — a control with a known-in-advance result runs
  first.
- **Verify the artifact by CONTENT, not by SHA delta.** After building, check the shipped
  binary contains your change (`strings`/symbol), because an unchanged SHA can mean "already
  correct" as well as "install did not land". Both mistakes happened here yesterday.
- **Never `ps | grep -q` under `set -o pipefail`** — `grep -q` exits on first match, `ps` takes
  SIGPIPE, `pipefail` propagates it and the condition reads FALSE while the process is alive.
  That misfire deployed a binary under a live run. Count into a variable.
- **The run contract requires the oracle prefix template's save snapshot.** Dropping the
  template gives `save_snapshot_manifest_sha256: path_absent` and a BLOCKED preflight before
  Wine. To get the language menu, use the prepared copy
  `artifacts/hk-prefix-template-NOLANG-inputtest` (oracle template minus the two language keys)
  and pass it as an **absolute path** — a bare name does not resolve.
- Mark every unproven statement `[HYPOTHESIS]`.

## Territory — three other agents are in this repo

- **YOURS:** `engine/wine/dlls/winemac.drv/**`, plus `tools/**` and `reports/**`.
- **FORBIDDEN:** `engine/wine/dlls/ntdll/unix/macrunner_hb.c`, `engine/hyperbridge/**`,
  `engine/dxmt/**` — the engine lane is live in those and they carry uncommitted work.
- **One title run at a time.** Check for a live run before launching; never `pkill`/`killall`.
- **No commits, no `git add`.**

## Termination

- `LOOP-STATUS: GOAL` — a keystroke or click provably reaches the game (menu selection moves,
  or the event trace shows delivery), with the defect named and the fix in the tree.
- `LOOP-STATUS: BLOCKED` — you need the operator at the keyboard for a manual confirmation, or
  a decision only he can make. Say so in one sentence; he is willing to test interactively.
