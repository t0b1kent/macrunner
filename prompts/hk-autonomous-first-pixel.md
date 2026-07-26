# AUTONOMOUS LANE: Hollow Knight — first visible pixel

You are running in an auto-loop. You will be relaunched in fresh threads until you write
`LOOP-STATUS: GOAL` or `LOOP-STATUS: BLOCKED` at column 0 of your progress file. Work
continuously; do not wait for a human between steps.

## Goal

Either (a) produce a **verified non-black frame** from Hollow Knight, or (b) **prove the
root cause** of the black frame with evidence that survives adversarial checking.

Current state (established, do not re-derive):
- Rendering is ALIVE: `DrawIndexed≈41807`, `Clear≈2433`, `OMSetRenderTargets≈15486`,
  `Present/Present1≈857`, `Performing automatic level start.` reached, faults/UNSUPPORTED/
  reject/HUP = 0.
- The Gfx command-ring storm exists but is NOT fatal — the stream resyncs and Present
  continues. It is a secondary defect, not the cause of black.
- The C0 sidechannel grid reported all sampled pixels black
  (`min=max=mean=-7.84313761e-06`, `nonzero=0`), BUT coverage was partial: 655360 of
  786432 coordinates written, 131072 left sentinel while the log still printed
  `coverage=full`. **That is a reporting defect — do not treat it as full-frame proof.**
- The grid reads the output of the specific C0 fragment draw, NOT the final backbuffer.
  A magenta control (C1/C2/C3) is required before trusting any grid verdict.

## Loop

Each iteration: pick ONE question → make the smallest instrument that answers it →
**prove the instrument honest** → run → read evidence → write verdict → next question.

### Already settled — do NOT re-run these, they cost a run each

- **C1 magenta control: PASSED.** Injected `rt0=1,0,1,1` reached the target and the encoder
  `rtv` equalled the readback address. Attachment, encoder and readback transport are
  proven honest. Do not re-litigate the instrument.
- **Grid coverage: fixed.** `coverage=full` no longer lies at 83%.
- **Shader translation: REFUTED as the cause** (`SHADER-LANE-AIRCONV-OFFLINE-VERDICT-20260726.md`,
  independently re-verified in a second thread): 152/152 real HK DXBC blobs translate with 0
  failures, a known-good `mov o0.xyzw, l(1,0,1,1); ret` control emits the constant to rt0,
  82/82 PS carry rt0 writes, 0 PS with `o0_writes==0`, discard parity 28/28 vs the DXBC scan.
- **Translator SSE matrix arithmetic MATCHES x86**; HK's MXCSR is `0x1f80` (masks + RNE, no
  FTZ/DAZ), host `FPCR=0x0`. Degenerate-matrix-by-arithmetic is weakened.

### THE NEXT RUN — do this one first

Run the **combined decisive run**, specified in full at
`reports/phase4-hollow-knight/HK-COMBINED-DECISIVE-RUN-SPEC.md`. Three open candidates
collapse into ONE run because a single already-built artifact carries every probe:
`engine/graphics/dist/dxmt/x86_64-windows/d3d11.dll` sha256
`336c76dff17411e76eb1b30e35161e32ba3a5f5d417f93034e66bb5ee96397c6` (verified by `strings`
to contain `MACRUNNER_HB_CAUSAL_CONTROL`, the combined mode string `C0,C2,C3`,
`_mr_c2_fragment_input_magenta`, `_mr_c3_vertex_reg1_magenta`, `MACRUNNER_DXMT_VS_CB_DUMP`,
`dxmt-vscb-totals`). `d3d11_shader.cpp:55` accepts `C0,C2,C3` as a native combined ladder
mode — three rungs in one run is supported, not a hack.

```sh
MACRUNNER_HB_CAUSAL_CONTROL=C0,C2,C3
MACRUNNER_DXMT_VS_CB_DUMP=1
MACRUNNER_DXMT_VS_CB_DUMP_MAX=512
# plus your presented-surface readback and its control
```

Two conditions that make or break it:
- **Sample LATE.** Every causal-target verdict so far was taken at `Present=4`/`DrawIndexed=3`.
  Full runs reach `Present≈857`/`DrawIndexed≈41807`. Four presents with three draws is Unity's
  boot window — **black there on a working machine too**. Gate on `Present>=200`, cumulative
  `DrawIndexed>=1000`, or after `Performing automatic level start.`
- **Re-take the control on THIS artifact.** The admitted `d3d11.dll` changes from
  `172ddbd…13f41` to `336c76df…`; never pair a control from the old DLL with a result from
  the new one. If preflight `prefix-sync-probe` prints `172ddbdb…`, re-sync; if the run falls
  back to `dxmt-builtin-overlay` (`f88868fc…`) both probes are silently absent and produce
  zero output — **zero output means probe not loaded, NOT "nothing happened".**

The spec's decision table tells you what each rung's outcome means. Follow it rather than
opening a new line of inquiry: with shaders refuted and the instrument proven, this run is
the shortest remaining path to the root cause.

## Hard gates — every one of these cost us hours; violating them wastes a 70-minute run

- **Verify the artifact actually landed.** After prefix-sync, check the SHA of the file in
  `system32` — not just that your build succeeded. A publish step can die silently.
- **Prove the tool before trusting the number.** A control that must produce a known
  non-trivial result (magenta) runs FIRST. "It compiled" is not evidence.
- **Build for the architecture the game loads.** x86_64-windows for the game's DLLs. We
  once put the Draw trace in the unused aarch64 copy and lost a day of measurements.
- **`not logged` ≠ `did not happen`.** Never conclude absence from a trace you have not
  proven active and unlimited. Sampling caps (first-N) make counts meaningless — emit
  aggregate totals.
- **Unity's own lines go to `launch.stdout`; ours to `launch.stderr`. Grep BOTH,
  case-SENSITIVE.** `grep -i present` matches `ddraw`/paths and has already produced two
  false findings.
- **Redirect and verify logs.** At +2 minutes confirm `launch.stdout` AND `launch.stderr`
  exist and are growing. If not, stop immediately — do not burn the budget.
- **Gate on progress, not on a timer.** Expect Present/dumps after the first real Present,
  not after N seconds. Under emulation the boot takes tens of minutes. Only if there is no
  Present by ~40 minutes is it an anomaly.
- **One title under JIT at a time.** Never start a run while another is live; never touch
  another lane's dist or prefix.
- **Scoped cleanup only** — by verified PID or prefix. Never a global `pkill`/`killall`.
- **No commits** unless the operator explicitly asked. Preserve work in the checkpoint
  instead (`reports/phase4-hollow-knight/checkpoints/…`).
- **Disk:** keep it clean, prune throwaway prefixes, but never delete
  `artifacts/hk-windows-oracle-prefix-template*` — `mr-clean --prune` has destroyed its
  save snapshot before.

## Reporting

- One report per iteration under `reports/phase4-hollow-knight/`, named for the question.
- Append one heartbeat line per step to your progress file.
- State evidence, not status: paste the actual log line / SHA / number. "blocked at X" with
  the exact blocker named is a valid result. Never fake forward progress; if a run is
  invalid, say so and why.
- Mark every unproven statement as `[HYPOTHESIS]`.

## Termination

- `LOOP-STATUS: GOAL` — a verified non-black frame, or a root cause proven with evidence
  that a skeptical reviewer cannot dismiss.
- `LOOP-STATUS: BLOCKED` — you need something only the operator can decide (a commit, a
  destructive action, hardware/quota). Name it in one sentence.
- Otherwise keep going: next question, next instrument, next run.
