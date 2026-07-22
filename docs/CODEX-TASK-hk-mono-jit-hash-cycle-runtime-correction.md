# TASK: HK Mono `jit_code_hash` causal runtime, launcher correction only

## Why this is the active wall

Do not run the proposed shader `C0/C2/C3` ladder again. The exact same
`ps_bef43b20` draw is already determined under checksum
`0x07194c46f282d32d`: `C0=BLACK`, `C1/C2/C3=MAGENTA`. The paired VS is a
faithful passthrough of Unity source vertex color `(0,0,0,5/255)`. Repeating
that ladder cannot isolate a new shader defect.

The current unresolved product wall is the post-scene stable loop captured at
guest PC `0x87ef2470d79`, statically mapped to
`mono_internal_hash_table_lookup(domain->jit_code_hash, method)`.

## Scope

Execute the already reviewed task
`docs/CODEX-TASK-hk-mono-jit-code-hash-cycle-causal.md`, Phases 1 through 3.
Reuse its accepted Phase-0 fixtures and static map. Do not rebuild, add probes,
change shaders, touch input/focus, create another preflight, or run a second HK
process.

The prior attempt is not product evidence: it died before Wine because its
detached shell lifecycle was not observable. Preserve that evidence and use a
new run root.

## Mandatory launcher correction

1. Copy the successful launcher bytes from
   `reports/phase4-hollow-knight/laneA-post-scene-guest-loop-capture-hash-retraction-20260722-2104/launch_hash_retraction_mapping.py`
   into the new run root.
2. Change only the literal `RUN_DIR` and `PREFIX` constants. Keep
   `ROOT`, `OLD_RUN_DIR`, `STAGED_DIST`, `DXMT_ROOT`, `EXE`,
   `load_child_environment()`, argv, and `os.execve()` byte-for-byte
   equivalent. Record the two-line semantic diff.
3. Start it as a foreground command-session:

   `python3 -u <new-run-root>/launch_hash_retraction_mapping.py`

   Use the command tool with a short initial yield so it returns a live session
   identifier. Do not use `&`, `nohup`, `setsid`, `disown`, a PTY wrapper, or a
   second launcher. Keep that command session open and poll it until nominal
   timeout or explicit scoped stop.
4. Redirect or retain both stdout and stderr through the command session. If it
   exits before `final-child.json`, record its real exit code and complete
   stderr. Do not retry.
5. Before LLDB, require the same live identity as the accepted 2104 run:
   `run-contract=READY`, child env `116/116`, forbidden inherited names `0`,
   exact raw `WINEDLLPATH` SHA-256 `cce9f264e671dcf4d6cf22894a40879802eb6ba1b8b941d1b9a8a78f6cc74432`,
   expected staged product hashes, and one owned HK child. Any mismatch stops
   the run without mutation.

Do not export shell bookkeeping names such as `CACHE`, `DIST`, `DXMT`, `EV`,
`EXE`, `OLD_FC`, `OLD_ROOT`, or `PREFIX`.

## Causal measurement

At `Performing automatic level start`, use the already accepted LLDB signal
policy and exact thread-restricted breakpoint from the reviewed task.

Read the live `jit_code_hash` bucket directly from guest memory. Each node is:

- key/method: `*(node + 0x0)`
- next link: `*(node + 0x8)`

Emit the ordered node list and classify it as finite, self-cycle, multi-node
cycle, unreadable, or capped. Also capture the indirect `next_value` callback
entry/return exactly as specified by the reviewed task.

Perform exactly one process-memory write only if a concrete repeated node proves
a cycle and all original safety gates pass. Cut only the repeated back-edge to
NULL, resume immediately, and measure for 180 seconds:

- `GetBuffer`, `Present`, `Present1`, Draw/encoder deltas
- manager/scene progress
- strict window captures at +10, +30, +60, and +180 seconds
- faults, rejects, `UNSUPPORTED`, and `MEMORY_FAULT`

No proven cycle means no write.

## Result

Update
`reports/phase4-hollow-knight/PIXEL-FIRST-MONO-JIT-HASH-CYCLE-CAUSAL-RESULT.md`
and seal one compact `NOT_GOLDEN` evidence directory. Use the verdict table from
the reviewed task. A non-black frame requires an immediate verified capability
snapshot. Do not create GOLDEN or commit a runtime-only outcome.

Cleanup only the owned prefix/staged clone. Preserve the translation cache and
confirm no HK/Wine/ABZU residue.
