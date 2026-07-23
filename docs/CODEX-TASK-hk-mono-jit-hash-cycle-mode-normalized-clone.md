# TASK: HK Mono `jit_code_hash` mode-normalized staged clone and causal run

## Authorization and correction

This is one narrow correction to the failed staged-dist restoration. It does
not authorize a new preflight system, a shader rerun, a product rebuild, a
production install, or any change to the historical evidence.

The strict comparison against the historical 2104 tree has now been completed
over all 4,732 entries. The source candidate is content-identical, but exactly
two mode bits differ:

| Path | Historical mode | Source-candidate mode |
| --- | --- | --- |
| `lib/wine/x86_64-windows` | `0555` | `0755` |
| `lib/wine/x86_64-windows/mono-profiler-hk_language.dll` | `0555` | `0755` |

The profiler regular-file SHA-256 is identical on both sides:
`7c62033f7315aaf14f31783c32b0ddb46a14757a3fb4e0e248c291c47b4a570b`.
No other path, type, size, SHA-256, symlink target, or mode differs.

Use these immutable inputs without modifying them:

- historical strict manifest:
  `reports/phase4-hollow-knight/laneA-post-scene-passive-stack-20260722-143030/STAGED-TREE.json`
- manifest identity:
  `9bb2d5f863ab6261a60b756adb98b11cdd1c02c062cdbaca58c2598a41871546`
- source candidate:
  `reports/phase4-hollow-knight/laneA-shader-value-production-c1-20260723-try1-064000/staged-dist`

## Phase 0: disposable exact-mode clone

1. Create one new disposable work root under
   `artifacts/_mr-run-aa-hk-mono-jit-hash-cycle-mode-normalized-*`. The clone
   must be inside that owned work root; do not write into either immutable path
   above.
2. Copy the source candidate to `<work-root>/staged-dist` with a preserving
   copy operation. Preserve regular files, directories, symlinks, and contents.
   Do not use a broad rebuild, rsync filter, or manual file selection.
3. Before any chmod, run the historical strict verifier over the clone and
   require exactly the two mode mismatches listed above and zero mismatches on
   every other comparison axis. If the mismatch set differs, stop before Wine.
4. Change the mode only of these two clone entries:

   ```sh
   chmod 0555 "$CLONE/lib/wine/x86_64-windows/mono-profiler-hk_language.dll"
   chmod 0555 "$CLONE/lib/wine/x86_64-windows"
   ```

   Do not use `chmod -R`, do not chmod any other path, and do not modify file
   contents, ownership, timestamps intentionally, symlinks, the source
   candidate, or the historical evidence.
5. Re-run the same strict lstat/no-follow verifier against `STAGED-TREE.json`.
   Require all 4,732 entries to match, including type, mode, size, regular-file
   SHA-256, and symlink target. Record both the before and after decision
   tables. Do not weaken the verifier to make modes informational.
6. Rehash the clone profiler and require the exact SHA-256 above. Record that
   the source candidate itself remains unchanged.

## Phase 1: identity-preserving launch

Start from the accepted foreground launcher at:

`reports/phase4-hollow-knight/laneA-post-scene-guest-loop-capture-hash-retraction-20260722-2104/launch_hash_retraction_mapping.py`

Create a launcher copy for this run. Relative to that accepted launcher, the
only semantic edits permitted are these three constants:

- `RUN_DIR` for this evidence root;
- `PREFIX` under this task's owned `artifacts/_mr-run-aa-*` root;
- `STAGED_DIST` set to this task's mode-normalized clone.

No other launcher line, environment construction, argv, timing, `os.execve()`,
or process-lifecycle behavior may change. Do not export orchestration variables
from the terminal into the parent shell. Do not use `&`, `nohup`, `setsid`,
`disown`, a detached wrapper, a PTY wrapper, or a second launcher. Keep the
foreground command session alive until the bounded run ends.

Run only after the strict clone verification passes. On the actual child,
require all of the following before LLDB acts:

- `run-contract=READY` and zero blockers;
- clean child environment `116/116` and forbidden inherited names `0`;
- raw `WINEDLLPATH` SHA-256
  `cce9f264e671dcf4d6cf22894a40879802eb6ba1b8b941d1b9a8a78f6cc74432`;
- accepted profiler SHA loaded from the per-run prefix.

Any failure is an explicit precondition result. Do not start a second runtime.

## Phase 2: existing causal test, no new instrumentation

If and only if Phase 1 is valid, execute Phases 1 through 3 of
`docs/CODEX-TASK-hk-mono-jit-code-hash-cycle-causal.md` unchanged:

1. Capture and classify the live `MonoDomain::jit_code_hash` chain at the
   exact loop.
2. Write one back-edge only after a concrete in-process cycle is proven by the
   existing read-before-write conditions.
3. Keep that same process alive for the existing bounded post-cut measurement.

Do not rerun C0/C1/C2/C3, add an observer, add a profiler feature, rebuild,
install production bytes, reset caches, send input, or perform focus work.

## Result and cleanup

Update
`reports/phase4-hollow-knight/PIXEL-FIRST-MONO-JIT-HASH-CYCLE-CAUSAL-RESULT.md`
with: strict clone verification before/after, launcher identity diff, child
identity, chain classification, any conditional one-edge write, and product
measurement. Seal compact NOT_GOLDEN evidence.

If a verified non-black game frame appears, create an immediate verified
capability snapshot before any subsequent change. Otherwise do not call the
result GOLDEN and do not commit product code. Preserve translation caches. After
evidence is sealed, remove only this task's owned prefix and disposable clone.
