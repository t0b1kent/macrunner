# HK black-frame root cause (2026-07-27)

**Class:** `VERIFIED_ROOT_CAUSE_NOT_FIXED`. The cause is proven and reproduced A/A.
Nothing is fixed; no pixel has been rendered yet.

## The finding

The black frame is **not a graphics defect**. Hollow Knight's managed scene bootstrap
halts inside the guest Mono runtime just after `Performing automatic level start.`; no
scene is created, so every presented frame is the cleared backbuffer `(0,0,0,255)`.

Proof is **differential against the Windows-Prism oracle**, not absence-based: the shared
marker `Game controller set to None.` is present in both (so the trace is live), and every
oracle marker after it is 0 in both of our runs, while `Screen position out of view
frustum` is 0 on the oracle and 19817 / 17927 on ours.

Reproduced A/A: two runs, presented-surface readback at Present ordinal 200, byte-identical
pixel hash `0xc770038f717d0383`, `black=786432 nonblack=0`.

## Exonerated layers — do not reopen without new evidence

Causal ladder C2/C3 both full-viewport MAGENTA at 655360/655360; 152/152 HK DXBC blobs
translate clean (rt0 writes 82/82, discard parity 28/28); composition binds the backbuffer
RTV 210/210 last-bound-before-Present1; Unity itself writes the black vertex colours
`(0,0,0,5/255)`; translator SSE matrix families match x86 with MXCSR `0x1f80`, host
`FPCR=0x0`.

## Contents

- `verdicts/` — the root-cause verdict, the combined decisive run result, the shader and
  composition refutations, the transform handoff, the run spec, the C1 control.
- `method/` — the lane prompts and the auto-loop scripts, so the autonomous setup that
  produced this is reproducible (failover chain codex → kimi → claude, behavioural
  quota detection, boundary-safe loop handover).
- `journals/` — the four lane progress journals.

## Regression detector for the next engine fix

Ordinal-200 presented-surface readback. Expected-black hash `0xc770038f717d0383`.
**A working Mono/JIT fix turns that readback non-black.** Acceptance checklist is the
oracle's post-level marker sequence: `Loaded saved language code 'EN'` →
`Making UI menu lean.` → `Opening_Sequence` → first visible pixel.

## Integrity

`SHA256SUMS` covers every file except itself; verified 20/20 at creation. A
credential-pattern sweep over the contents found nothing.
