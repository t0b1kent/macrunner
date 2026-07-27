# Decisive control: does the presented drawable reach the screen?

Date: 2026-07-27. Coordinator spec. Run this as the next run after run6.

## The contradiction it resolves

Same run (`laneA-smc-reverify-6-long-try1-062846`), same moment:

- ordinal-200 presented-surface readback: `hash=0xf7ea9ca044b2fdb5 black=774464
  nonblack=11968`, greyscale to full white — byte-identical to run5, so reproducible.
- all 14 `window-capture-*.bmp`, **including `window-capture-readback.bmp` taken at readback
  time**: full scan, `786432/786432` pixels exactly `0`, `max=0`.

Reproducible across two runs proves the readback is **deterministic**. It does not prove the
buffer is what reaches the screen — a deterministic artifact is still deterministic. The C0
grid taught us this exact lesson: stable numbers from the wrong buffer, and only a magenta
control settled it.

## The control already exists — verified in source, not assumed

`engine/dxmt/src/winemetal/unix/winemetal_unix.c:1557-1584`:
`MACRUNNER_HB_CAUSAL_PRESENT_SURFACE_CONTROL=C1` builds a render pass on the **presented
drawable's own texture**, `loadAction = MTLLoadActionClear`,
`clearColor = MTLClearColorMake(1.0, 0.0, 1.0, 1.0)` (magenta), `storeAction = Store`, on the
same command buffer as the present, and logs
`macrunner-hb-causal-present-surface: phase=c1-inject result=ok texture=%p value=1,0,1,1`.
Only `C1` and `phase==1` are accepted; anything else logs `reason=only-C1-is-supported`.

So the control writes a known colour into the exact surface being presented. That is the
right instrument for this question.

## Window capture is a genuinely independent path

`scripts/window-capture-verdict.sh` finds the window via `CGWindowList` and captures with
macOS `screencapture`. It does not go through Metal, DXMT or our readback, so agreement
between it and the readback is meaningful evidence, not a tautology.

## Prove the capture tool before trusting its black

**I verified `screencapture` itself works on this machine**: a full-screen probe returned
59809 of 61229 sampled pixels non-zero, `max=255`. So an all-black capture is *not* simply a
dead tool.

**But macOS Screen Recording permission is granted per responsible app**, and my probe ran
from a different process lineage than the lane's nohup'd agent. So the lane must still prove
*its own* capture path: capture any known non-black window once from the same process
lineage the run uses, and show a non-zero pixel count. Until that passes, an all-black
capture from that path means nothing — `not captured != not displayed`.

## The run

Same admitted artifacts and run contract as run6, one variable changed:

```sh
MACRUNNER_HB_CAUSAL_PRESENT_SURFACE_CONTROL=C1
# plus the presented-surface readback exactly as run5/run6 had it
```

Capture the window at and around the ordinal-200 readback, and keep both `launch.stdout` and
`launch.stderr`. Confirm `phase=c1-inject result=ok` actually appears — if the inject never
fired, the run says nothing.

**Also persist the raw ordinal-200 RGBA this time.** It has been asked for twice. Nobody has
looked at the 11968 pixels; a 3 MB file ends that.

## Decision table

| readback | window capture | verdict |
|---|---|---|
| magenta | magenta | The capture path is fine and the drawable does reach the screen. Then the earlier all-black windows mean the game really was rendering black at those moments, and the `nonblack=11968` readback is measuring something that is *not* the presented image — chase the readback's source. |
| magenta | black | **The drawable never reaches the screen.** A new compositing gap at the macOS/Metal layer, below everything D3D-side that was already cleared (backbuffer RTV bound 210/210 last-before-`Present1`). This becomes the next blocker and it is a good one — it is narrow and local. |
| black | black | The C1 inject did not take effect on the surface the readback samples. Instrument problem first: check `phase=c1-inject result=ok` fired and that the readback samples the same texture pointer the control cleared. |
| black | magenta | Contradiction in the other direction: the control reached the screen but not the readback, so the readback samples a different surface. Every readback verdict, including `nonblack=11968`, is void until the source is fixed. |

## What must not happen

- Do not run this on top of another title — one JIT run slot.
- Do not change two variables. Same build, same contract as run6, control knob only.
- Do not report a verdict without `phase=c1-inject result=ok` in the log and the capture
  tool's own liveness proof.
