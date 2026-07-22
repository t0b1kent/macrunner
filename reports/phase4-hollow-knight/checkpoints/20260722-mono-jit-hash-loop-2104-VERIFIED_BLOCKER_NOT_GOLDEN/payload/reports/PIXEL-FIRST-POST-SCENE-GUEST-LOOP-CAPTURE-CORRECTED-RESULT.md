# Pixel-First Post-Scene Guest Loop Capture Corrected Result

Classification: `UNKNOWN_IDENTITY_DRIFT`
Golden status: `NOT_GOLDEN`
Prompt: `docs/CODEX-TASK-hk-post-scene-guest-loop-capture-corrected.md`
Prompt SHA-256: `13fb3788ee9f10531a3499160f57c8ee682f6901a6b6f487457234361b197ee2`

## Verdict

The corrected LLDB signal-policy dry run passed, but the single authorized HK
mapping runtime failed live admission before the scene marker. The child
environment had `129` entries instead of the required `116`. This consumed the
one corrected mapping runtime authorization; no retry was performed.

No valid post-scene sample/LLDB capture exists from this corrected attempt. No
IR pair, exact-byte fixture, source fix, validation runtime, checkpoint, commit,
or GOLDEN was produced.

## Invalid Prior Attempt

Preserved unchanged:
`reports/phase4-hollow-knight/laneA-post-scene-guest-loop-map-20260722-162042`

Prior report SHA-256:
`d30ab769623060912ae1eb339cd9b75a03ac18fc0b1a6d3f444ff500021f22ce`

## Corrected Attempt Evidence

Evidence root:
`reports/phase4-hollow-knight/laneA-post-scene-guest-loop-capture-corrected-20260722-1812`

Key hashes:

- `PHASE0-CORRECTED-PROOF.json`: `d639c9b272463d0febab23167c1a26ba41bc482de7ecaabcdb88b280e9c7a828`
- `PRE-RUN-IDENTITY.json`: `53e1b71bf08a3186220ac6e8448cb9f1450250cf81b6ab7afb852e3be0685c0d`
- `LIVE-ADMISSION.json`: `84a2886130fce0feee89df801f4bddee103d72b9e0ed16088944ba58dfed92c6`
- `run.log`: `5809bc00ff38860f05748e23d87903e2c8b7160fda799ee8f6f3fcb734dfacb6`
- `final-child.json`: `61b9551d68680b00e48705cfad106dd5bf98e3c3dd5720b1d8e6fad7d5fbec17`

`final-child.json` contains raw inherited environment values and must not be
committed or uploaded.

## Corrected LLDB Dry Run

The corrected build-only fixture proof passed before HK launch:

- signal policy table PASS for `SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGUSR1`, `SIGUSR2`:
  `PASS/STOP/NOTIFY = true/false/false`
- fixture `SIGSEGV` handler returned: `sigsegv_handler_count=1`
- LLDB did not stop on the handled fault
- exact thread-restricted `hb_jit_helper_exec_two_block_loop` breakpoint hit
- read-only capture wrote valid headers/IR with `required_read_errors=[]`
- timeout path detached and left the fixture alive before scoped cleanup

## Product Bytes And Preflight

Pre-run checks passed:

- collisions: `0`
- staged typed tree: `PASS`, 4732 entries, no changed entries versus accepted staged oracle
- Unix `ntdll.so`: `9a3f20b426adeeb86d70c06d0b7cfc9e687838e6eace5e43c282a19eb0363fc7`
- `aarch64-windows/ntdll.dll`: `3863660ac0b5f57c106d610d6a88d897a89a6c0ab626f890e66b4d572f02de4d`
- actuator: `7c62033f7315aaf14f31783c32b0ddb46a14757a3fb4e0e248c291c47b4a570b`
- child `WINEDLLPATH` hash target: `cce9f264e671dcf4d6cf22894a40879802eb6ba1b8b941d1b9a8a78f6cc74432`
- cache policy: preserve current warmed cache, no reset
- cache pre-run SHA-256: `8e9e944909539475e25a4696c8812389f040beada56dd5f0f31066f853127bc8`

## Live Admission Failure

The child identity failed:

- run-contract status: `READY`
- blockers: `0`
- child PID: `21359`
- child env count: `129`
- expected child env count: `116`
- child `WINEDLLPATH` entries: `5`
- child `WINEDLLPATH` SHA-256:
  `cce9f264e671dcf4d6cf22894a40879802eb6ba1b8b941d1b9a8a78f6cc74432`
- product-forbidden vars: absent
- scene marker count: `0`

The 13 extra names were inherited from the Codex/terminal parent environment:
`API_KEY_DEEP`, `API_TOKEN`, `BROWSER_USE_AVAILABLE_BACKENDS`, `CODEX_CI`,
`CODEX_THREAD_ID`, `GH_PAGER`, `GIT_PAGER`, `LC_ALL`,
`NODE_REPL_TRUSTED_BROWSER_CLIENT_SHA256S`, `NODE_REPL_TRUSTED_CODE_PATHS`,
`OPENAI_API_KEY`, `PAGER`, `SSH_AUTH_SOCK`.

No values are reported here.

## Stop And Cleanup

Because the required identity was not byte-equivalent, the run was stopped
fail-closed using scoped `wineserver -k` for only:
`artifacts/_mr-run-aa-hk-post-scene-guest-loop-capture-corrected-20260722-1812`

Final state:

- runner returned after scoped stop
- prefix absent
- scoped HK/Wine process residue: `0`
- disposable staged clone removed
- evidence run directory preserved
- translation cache preserved
- disk headroom after cleanup: `55Gi`

## Required Future Correction

A future authorized attempt must launch from the imported sealed child
environment, not from `os.environ.copy()`. The launch environment should be
constructed from the 116-entry imported template plus the allowed identity path
overrides, with parent `WINEDLLPATH` reduced to the accepted four entries before
`mr-run.sh` prepends the DXMT root.

## Env-Clean Continuation

Task file:
`docs/CODEX-TASK-hk-post-scene-guest-loop-capture-env-clean.md`

Task SHA-256:
`d2a316a98ede3442662d1c0db5dfa2dbd8d8c1d7df156c5136996f4d4f7aa52d`

Evidence root:
`reports/phase4-hollow-knight/laneA-post-scene-guest-loop-capture-env-clean-20260722-1900`

The one-line launcher correction was applied in a run-local copy only:

```python
env = load_child_environment()
os.execve(argv[0], argv, env)
```

No source rebuild, product byte change, new observer, input, focus, activation,
or shader work was performed.

Phase 1 static proof:

- `PHASE1-CLEAN-ENV-PROOF.json` SHA-256:
  `1e47c6ccf7e7187835e3a6a0b7b8664a170a7f2c55e1199c08a086bf025fff03`
- `PRE-RUN-IDENTITY.json` SHA-256:
  `177fc66c06dff2dd71f7e1abb8da94a5ca4474b2380d90169267f8ec8b06a6b2`
- launcher SHA-256:
  `8289ef43e6a0db69f6fb55f18f2768a7716501f5d290ceacc1c6741d70f69835`
- static child env name count/hash:
  `116`, `9793680dfb7541b522811065ad2f1a6693cb0b6d64147366288bfbe6014ef532`
- static forbidden 13 names: `0`
- static expected child `WINEDLLPATH` hash:
  `cce9f264e671dcf4d6cf22894a40879802eb6ba1b8b941d1b9a8a78f6cc74432`

Runtime admission result:

- `final-child.json` SHA-256:
  `27f7ebd18c9b54a222ea5c83be4d9756cea948ebe181b05aa8616dd0ef2f4b22`
- `run.log` SHA-256:
  `b3cb8784c01f9f45420ef02925d92fae43aedf51db03a4aaa8ed79eb99c160c0`
- live child env name count/hash:
  `116`, `9793680dfb7541b522811065ad2f1a6693cb0b6d64147366288bfbe6014ef532`
- live forbidden 13 names: `0`
- live child `WINEDLLPATH` count/unique/hash:
  `5`, `5`,
  `6ae1e2285c346316c822b24c8c45afb2cdc1e76ad2a519a0b5b53dd0bc0449d1`
- accepted source-template child `WINEDLLPATH` hash:
  `6ae1e2285c346316c822b24c8c45afb2cdc1e76ad2a519a0b5b53dd0bc0449d1`
- run-contract save-path variable names in child env: `0`
- `MONO_ENV_OPTIONS` in child env: present
- `Performing automatic level start`: `0`
- `CreateSwapChainForHwnd` / `GetBuffer` / `Present`: `0` / `0` / `0`
- `UNSUPPORTED` / `MEMORY_FAULT` / reject counters in `run.log`: `0` / `0` / `0`
- `termination.json`: absent because the run was stopped at live admission

Verdict for this continuation:

`UNKNOWN_IDENTITY_DRIFT`.

The clean-env launcher removed the 13 inherited Codex/terminal names, but the
live child state did not match the Phase 1 sealed admission contract. The live
child `WINEDLLPATH` matched the imported 116-name source template hash
`6ae1...`, while Phase 1 expected `cce9...`; the required run-contract save-path
field was also absent from the actual child environment. Per the handoff, the
run was stopped before waiting for the scene marker and before LLDB capture.

Cleanup:

- scoped `wineserver -k` rc: `0`
- owned prefix:
  `artifacts/_mr-run-aa-hk-post-scene-guest-loop-capture-env-clean-20260722-1900`
  absent
- disposable staged clone:
  `reports/phase4-hollow-knight/laneA-post-scene-passive-stack-20260722-143030/staged-dist`
  removed
- translation cache preserved
- scoped HK/Wine residue after cleanup: `0`
- disk headroom after cleanup: `56Gi`

No LLDB guest loop capture, source patch, validation runtime, snapshot, commit,
or GOLDEN classification was produced.

## Next Required Correction

Before any future runtime authorization, the static admission proof must be
derived from the actual `mr-run.sh` child environment semantics and must include
the required run-contract save-path field. The next prompt should resolve the
`cce9...` versus `6ae1...` contradiction explicitly; this attempt proves only
that the inherited-secret leak was removed, not that the mapping capture is
admissible.

## Hash Adjudicator Retraction

The `UNKNOWN_IDENTITY_DRIFT` verdict above is retracted. The stopped 1900 attempt
is reclassified as:

`INVALID_ADJUDICATOR / PRODUCT_NOT_MEASURED / NOT_GOLDEN`.

The original text is preserved for audit history. The error was in the local
hash adjudicator, not in the child environment. Canonical replay against:

- `reports/phase4-hollow-knight/laneA-post-scene-guest-loop-capture-env-clean-20260722-1900/final-child.json`
- `reports/phase4-hollow-knight/laneA-post-scene-guest-loop-map-20260722-162042/final-child.json`

proved:

- live child `WINEDLLPATH` equals the accepted 116-name source-template value
  byte-for-byte
- raw UTF-8 bytes: `900`
- canonical raw-value SHA-256:
  `cce9f264e671dcf4d6cf22894a40879802eb6ba1b8b941d1b9a8a78f6cc74432`
- colon entries / unique entries: `5` / `5`
- noncanonical joined-newline SHA-256:
  `6ae1e2285c346316c822b24c8c45afb2cdc1e76ad2a519a0b5b53dd0bc0449d1`

`6ae1...` is SHA-256 of the five `WINEDLLPATH` entries joined by newline plus a
final newline. It must not be used for the raw-value child environment contract.

The 1900 `run-contract.json` was also valid for the requested gate:

- status: `READY`
- blockers: `0`
- save snapshot: `PRESENT`, five files, 11121 bytes, inventory SHA-256
  `442d9b2310ccc6b88ab24a554a4b70c9747607f52469f275a0bbc2c88dcac919`
- config manifest: `PRESENT`, 18 files, 681990 bytes
- data manifest: `PRESENT`, 1761 files, 5196290140 bytes

`MACRUNNER_RUN_CONTRACT_SAVE_PATH` is not a child-env requirement for this
contract. The authoritative save/config/data gate is `run-contract.json`.

Phase 0 proof:
`reports/phase4-hollow-knight/laneA-post-scene-guest-loop-capture-hash-retraction-20260722-2104/PHASE0-HASH-RETRACTION-PROOF.json`.

## Hash-Retraction Fresh Runtime

Evidence root:
`reports/phase4-hollow-knight/laneA-post-scene-guest-loop-capture-hash-retraction-20260722-2104`

Pre-run identity:

- `PRE-RUN-IDENTITY.json` SHA-256:
  `7fb621fb34cbfa201bf53c585da27aec2d2d1b8206b9405282baebc4ee83f69d`
- launcher SHA-256:
  `df877d9b39ee1d4a79f755fec14170d23aa7017b6f21fbb3921399c109e9f2e5`
- LLDB scripts:
  `c2c0bab9af56885602ed98e227d409902f1f3d9ce41a5e98c3286145ce3192af`,
  `0bea31fe7a8e2b72a5c089d78699f2591bcf59c0dae73b81db968d0bda523154`
- product hashes matched the task for `ntdll.so`, ARM64X `ntdll.dll`,
  x86_64 `ntdll.dll`, `winemac.so`, and actuator
  `mono-profiler-hk_language.dll`
- translation cache policy: preserved current warmed state, not reset

Live admission:

- `final-child.json` SHA-256:
  `3034ead4e12823c62219c48d43bdd4cf528c9d88e0147ccfde1bdf631551179d`
- `run-contract.json` SHA-256:
  `b284001013975508ab16f4d97fffccdf55ec5b42354434a7e3bb16fffc551dbf`
- child `WINEDLLPATH` raw bytes: `900`
- child `WINEDLLPATH` raw SHA-256:
  `cce9f264e671dcf4d6cf22894a40879802eb6ba1b8b941d1b9a8a78f6cc74432`
- env names: `116/116`, sorted-name SHA
  `9793680dfb7541b522811065ad2f1a6693cb0b6d64147366288bfbe6014ef532`
- forbidden 13 inherited names: `0`
- `run-contract.json`: `READY`, blockers `0`, save/config/data manifests
  `PRESENT`

Runtime/capture:

- `run.log` SHA-256:
  `408258f4aa709ec3b86f060401e977650539b3bc228d9d632b696ec7a0574b12`
- automatic-level marker: `1`
- pre-marker `Present` / `GetBuffer` / draw / encoder:
  `48` / `1` / `0` / `1`
- post-marker `Present` / `GetBuffer` / draw / encoder:
  `0` / `0` / `0` / `0`
- `UNSUPPORTED` / `MEMORY_FAULT` / reject: `0` / `0` / `0`
- passive sample SHA-256:
  `0d440d4476165ea3f225fdd58c22bfd08567e43126dd1a6e4563b82e12bd3694`
- selected thread: `Thread_5395781`, with sample counts
  `hb_jit_helper_exec_two_block_loop=133`, `mem_read=40`
- LLDB supervisor SHA-256:
  `f1194d6591360cdb7a588dc2d14cec4ce6d19b1653f5e1c6e5edadecef666334`
- LLDB capture SHA-256:
  `15b2e492137387ef1fd87a5f06a6a8114f46cf6d7ccd96ebef801cf49de14030`
- LLDB verdict: `PASS`; TID matched; function
  `hb_jit_helper_exec_two_block_loop`; stop reason `one-shot breakpoint 1`;
  signal policy OK; required read errors `0`

Captured guest loop:

```text
ctx->pc = 0x87ef2470d79
x0(ctx) = 0xa14a94000
x1(first block) = 0xa210775c0
x2(second block) = 0xa2109ad00

first block guest=0x87ef2470d79 instr_count=2
  0x87ef2470d79: 48 3b c6    cmp rax, rsi
  0x87ef2470d7c: 74 20       je 0x87ef2470d9e
IR:
  CMP RAX, RSI
  Jcc E -> 0x87ef2470d9e

second block guest=0x87ef2470d7e instr_count=2
  0x87ef2470d7e: 48 8b cb    mov rcx, rbx
  0x87ef2470d81: ff 57 10    call qword ptr [rdi + 0x10]
IR:
  MOV RCX, RBX
  CALL mem64[RDI + 0x10]

following captured guest window:
  0x87ef2470d84: 48 8b 18    mov rbx, qword ptr [rax]
  0x87ef2470d87: 48 85 db    test rbx, rbx
  0x87ef2470d8a: 75 e7       jne 0x87ef2470d73
```

Adjudication:

`CAPTURE_PASS_STABLE_GUEST_LOOP_NOT_GOLDEN`.

The captured bytes and IR agree for the two-block helper path. The helper executes
the captured IR through the interpreter inside the optimized loop, so this does
not prove a decoder or execution-family defect. Per the task, no source patch was
made and no validation runtime was run. The remaining boundary is the Unity
bootstrap state transition that should exit this loop: either the `cmp rax,rsi`
equality branch at `0x87ef2470d7c` or the downstream callback/list transition
after `call [rdi+0x10]`.

Machine-readable adjudication:
`reports/phase4-hollow-knight/laneA-post-scene-guest-loop-capture-hash-retraction-20260722-2104/GUEST-LOOP-CAPTURE-ADJUDICATION.json`.

Cleanup:

- scoped `wineserver -k` rc: `0`
- prefix auto-removed by runner
- translation cache preserved
- no source patch, validation runtime, snapshot, commit, or GOLDEN
