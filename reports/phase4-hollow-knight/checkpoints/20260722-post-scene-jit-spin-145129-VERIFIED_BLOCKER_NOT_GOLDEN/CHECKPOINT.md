# HK Post-Scene JIT Spin Checkpoint

Classification: `VERIFIED_BLOCKER_NOT_GOLDEN`

Machine verdict: `STACK_CLASSIFIED_PASS`

Source run:

`reports/phase4-hollow-knight/laneA-post-scene-passive-stack-corrected-20260722-145129`

Report:

`reports/phase4-hollow-knight/PIXEL-FIRST-POST-SCENE-STACK-CLASSIFICATION.md`

This compact checkpoint advances the prior post-scene submission-stall floor to
a native execution boundary. After `Performing automatic level start.`, the
same live Hollow Knight process remained CPU-active while post-boundary
`GetBuffer`, `Present`, `Present1`, draw, and encoder counts stayed at zero.
Two independently captured samples at observed offsets +58 s and +140 s agree
on Unity guest producer thread `4704952` (`0x47cab8`) executing through
`hb_jit_runtime_run`, generated ARM64 code, and
`hb_jit_helper_exec_two_block_loop -> exec_instr -> mem_read`. The macOS main
thread remained in a normal Cocoa run loop. No unsupported opcode, memory fault,
reject, or HUP was recorded, and both window captures remained black.

The generated host PC `0x11d397e38` is evidence from this run only. It must not
be reused as a selector in another run because JIT native addresses are
ephemeral. The next boundary is the guest-address pair and IR/memory condition
inside the two-block helper loop.

This is a blocker floor, not a visible-pixel milestone. It intentionally
excludes full logs, raw `final-child.json`, expected child environment values,
staged runtime trees, shader dumps, disposable prefixes, and translation-cache
bytes.
