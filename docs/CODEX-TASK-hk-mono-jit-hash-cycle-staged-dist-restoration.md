# TASK: HK Mono `jit_code_hash` staged-dist restoration and causal run

## Authorization

The prior runtime-correction did not reach Wine because its historical
`STAGED_DIST` directory was deleted. This is a launcher-identity correction,
not a new shader or preflight task.

The replacement below is proven byte-identical to the deleted staged dist by
the preserved typed-tree manifest:

- historical manifest:
  `reports/phase4-hollow-knight/laneA-post-scene-passive-stack-20260722-143030/STAGED-TREE.json`
- manifest identity: `9bb2d5f863ab6261a60b756adb98b11cdd1c02c062cdbaca58c2598a41871546`
- entries: `4732`
- verified replacement:
  `reports/phase4-hollow-knight/laneA-shader-value-production-c1-20260723-try1-064000/staged-dist`
- replacement typed-tree digest: `fb32200b910e7f41b9669bb05b9fb538dde5c5683ea5d984737bf3657b5f1a0a`

Every historical entry matches the replacement in path, type, mode, size,
regular-file SHA-256, and symlink target. The two runner binaries and all
native product hashes also match the accepted 2104 identity.

## Exact permitted change

Start from the foreground launcher created for
`docs/CODEX-TASK-hk-mono-jit-hash-cycle-runtime-correction.md`.

Change exactly one additional launcher constant:

```python
STAGED_DIST = ROOT / "reports/phase4-hollow-knight/laneA-shader-value-production-c1-20260723-try1-064000/staged-dist"
```

The only allowed launcher semantic changes relative to the accepted 2104
launcher are now `RUN_DIR`, `PREFIX`, and this proven identical `STAGED_DIST`
literal. Do not change any other line, environment construction, argv, or
`os.execve()` behavior.

## One runtime

1. Before launch, re-verify all 4,732 entries of the replacement against the
   historical `STAGED-TREE.json`. Any mismatch stops before Wine.
2. Use the exact foreground command-session launch already specified by the
   runtime-correction task. Keep the command session alive. Do not use `&`,
   `nohup`, `setsid`, `disown`, a PTY wrapper, or a second launcher.
3. Require `run-contract=READY`, child environment `116/116`, forbidden
   inherited names `0`, and raw `WINEDLLPATH` SHA-256
   `cce9f264e671dcf4d6cf22894a40879802eb6ba1b8b941d1b9a8a78f6cc74432`
   before LLDB.
4. Execute Phases 1 through 3 of
   `docs/CODEX-TASK-hk-mono-jit-code-hash-cycle-causal.md` exactly:
   read and classify the live hash chain; write one back-edge only after a
   concrete cycle is proved; then measure 180 seconds.

No shader C0/C2/C3 rerun, rebuild, production install, input/focus work,
cache reset, retry, or second HK process is authorized.

## Result

Update `reports/phase4-hollow-knight/PIXEL-FIRST-MONO-JIT-HASH-CYCLE-CAUSAL-RESULT.md`
and seal compact NOT_GOLDEN evidence. A non-black frame requires an immediate
verified capability snapshot. Preserve the translation cache and remove only
the owned prefix and staged clone after completion.
