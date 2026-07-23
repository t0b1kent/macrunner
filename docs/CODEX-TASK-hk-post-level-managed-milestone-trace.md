# TASK: HK post-level managed milestone trace

## Objective

The latest valid capture removes the current generic HyperBridge/JIT-loop
hypothesis: the selected main runtime thread executed the captured guest block
correctly and left it. The product now reaches `Performing automatic level
start.`, but the expected Windows continuation is absent from the bounded Mac
run. Trace that one managed boundary directly.

The native Windows oracle records this order after automatic level start:

1. `GameManager.LevelActivated`
2. `UIManager.MakeMenuLean`
3. `OpeningSequence.OnChangingSequences`

This task adds one default-off, no-allocation observer trace for exactly those
three methods. It does not invoke methods, write managed state, send input,
force scene activation, attach LLDB, or modify any JIT/DXMT/Wine code.

## Static identities

Before changing the observer, independently verify and record the class,
method, arity, metadata token, and RVA for each target from the same managed
assembly used by the staged game:

| Class | Method | Arity | Expected token | Expected RVA |
| --- | --- | ---: | --- | --- |
| `GameManager` | `LevelActivated` | 0 | `0x06000D53` | `0x4C870` |
| `UIManager` | `MakeMenuLean` | 0 | `0x06000F17` | `0x52560` |
| `OpeningSequence` | `OnChangingSequences` | 0 | `0x06000355` | `0x194A4` |

If the assembly identity or any static identity differs, stop before build and
report `TRACE_STATIC_IDENTITY_MISMATCH`. Do not substitute a similar method.

## Narrow implementation

Modify only `tools/hk_language_observer/hk_language_observer.c` and its focused
tests/build inputs as required.

1. Add `MACRUNNER_HB_POST_LEVEL_MILESTONE_TRACE=1`, default off. Parse it once
   during observer initialization. The normal language observer behavior must
   remain byte-for-byte equivalent when the flag is absent.
2. Extend the exact method classifier/filter so this flag instruments only the
   three identities above. Request enter and leave callbacks for these three
   methods only; keep all other methods at `NONE` through this new path.
3. Emit one bounded, allocation-free record per entry and normal leave:
   `post-level-milestone phase=enter|leave class=<...> method=<...> arity=0
   managed_tid=<...> sequence=<...>`.
   Include the static token/RVA in the startup/arming record, not a guessed
   runtime value.
4. Do not enable allocation profiling, register an allocation callback, call
   `mono_runtime_invoke`, use direct-confirm, retain a new gchandle, or write a
   field in this mode. The trace is observation only.
5. Correct the `confirmedLanguage` reporting contract while touching this code:
   it is a Boolean field. A field read may report only `true`, `false`, or
   `unavailable` plus a concrete reason. Never render an unavailable read as
   `-1` or infer that `ConfirmLanguage` failed from that sentinel. This change
   must not change managed state or invoke `ConfirmLanguage`.
6. Give this exact build an unambiguous startup marker such as
   `post-level-milestone-trace=armed`; `allocations=excluded` by itself is not
   deployment proof because stock observer builds emit it too.

## Build-only gate

Run focused tests before any game process exists:

1. Default-off fixture: the three methods receive no callback requests and no
   post-level records.
2. Exact-identity fixture: only the three class/method/arity pairs above are
   accepted; wrong class, arity, or similarly named method is rejected.
3. Callback fixture: each accepted method requests enter plus leave; no other
   method is broadened by the new flag.
4. No-allocation/no-actuation fixture: the new flag path cannot reach
   allocation profiling, allocation callbacks, `mono_runtime_invoke`, direct
   confirm, field writes, or input/activation code.
5. Boolean-read fixture: unavailable field reads are emitted as
   `unavailable`, never `-1`.
6. Produce two scratch PE builds. Their hashes must match. Seal source, PE,
   focused-test output, and static-identity evidence before runtime.

If any build gate fails, stop. Do not run the game, revise the task, or create a
new apparatus layer.

## One runtime

Use the already accepted HK launch identity, not a new launcher design.

1. Recreate the disposable mode-normalized clone exactly as in
   `docs/CODEX-TASK-hk-mono-jit-hash-cycle-mode-normalized-clone.md`:
   immutable source and historical tree, only the two established clone modes
   changed, strict verifier `4732/4732` after correction.
2. Reuse the accepted foreground launcher from
   `laneA-post-scene-guest-loop-capture-hash-retraction-20260722-2104`.
   Relative to it, alter only `RUN_DIR`, `PREFIX`, and `STAGED_DIST`. Do not
   export orchestration variables, use a detached wrapper, `&`, `nohup`,
   `setsid`, `disown`, or a PTY wrapper.
3. Deploy the sealed scratch observer PE atomically into the owned per-run
   prefix `system32` before Mono initialization. Prove both its SHA-256 and the
   new `post-level-milestone-trace=armed` marker before accepting runtime data.
   Do not change the working dist or production prefix.
4. Require live admission before interpreting telemetry: `run-contract=READY`,
   child environment `116/116`, forbidden inherited names `0`, raw
   `WINEDLLPATH` SHA-256
   `cce9f264e671dcf4d6cf22894a40879802eb6ba1b8b941d1b9a8a78f6cc74432`,
   and the sealed scratch observer SHA-256.
5. Wait for exact `Performing automatic level start.`. Keep the game alive for
   at least 120 seconds after that marker. Take passive per-window captures at
   marker `+10`, `+30`, `+60`, and `+120` seconds. Do not inject input or add a
   draw observer. A zero `Draw*` count in the existing swaptrace is
   inconclusive and must not appear as an inference in the verdict.
6. One runtime only. Any launch/admission/deployment failure is an explicit
   `TRACE_INVALID` result, with the exact failed precondition. No retry.

## Decision table

| Observation | Verdict |
| --- | --- |
| Marker absent, deployment proof absent, identity mismatch, or live admission failure | `TRACE_INVALID` |
| Automatic-level marker present but `LevelActivated` entry absent | `TRACE_INVALID` |
| `LevelActivated` entry without normal leave | `LEVELACTIVATED_EXECUTION_BOUNDARY` |
| `LevelActivated` leaves but `MakeMenuLean` is absent in the post-marker window | `POST_LEVEL_DISPATCH_DIVERGENCE` |
| `MakeMenuLean` leaves but `OnChangingSequences` is absent in the post-marker window | `OPENING_SEQUENCE_DISPATCH_DIVERGENCE` |
| All three leave normally and captures remain black | `POST_LEVEL_MANAGED_PASS_RENDER_BOUNDARY` |
| Any verified non-black game-window capture | `PIXEL_PASS` |

The result must distinguish an absent call from a call that entered but did not
leave. Do not infer a generic Mono, JIT, input, shader, or Draw conclusion
beyond this table.

## Deliverables and cleanup

Write:

- `reports/phase4-hollow-knight/PIXEL-FIRST-POST-LEVEL-MANAGED-MILESTONE-TRACE-RESULT.md`
- a compact run evidence directory with source/PE hashes, static mapping,
  startup marker, exact callback sequence, admission data, capture hashes, and
  cleanup proof.

If all three methods leave normally, create a compact
`VERIFIED_CAPABILITY_NOT_GOLDEN` checkpoint before any later risky change. If a
non-black capture appears, create the verified pixel snapshot immediately. In
all other cases retain only compact `NOT_GOLDEN` evidence. Do not commit product
code, install to the working dist, alter translation caches, or touch ABZU.
