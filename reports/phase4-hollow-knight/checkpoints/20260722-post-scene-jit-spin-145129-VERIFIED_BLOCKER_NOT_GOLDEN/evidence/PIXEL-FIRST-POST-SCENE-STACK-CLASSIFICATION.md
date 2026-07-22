# Pixel-First Post-Scene Passive Stack Classification

Classification: `NOT_GOLDEN`

Machine verdict: `STACK_CLASSIFIED_PASS` (corrected attempt)

Preserved first-attempt verdict: `UNKNOWN`

## Scope

The first authorized Hollow Knight runtime was stopped fail-closed before the
automatic-level-start boundary because its effective child environment did not
match the sealed 116-field baseline. A separately authorized corrected runtime
then used the proved four-parent-root to five-child-root transformation and
took exactly two passive macOS `sample` captures. The corrected run reached a
determined native stack class without a source edit, rebuild, input,
focus/activation action, shader work, production install, commit, snapshot, or
GOLDEN action.

Task SHA-256:
`4423977b12583535634dde79041bf761cdcd0e4b5b64f983f33f40934aa1d9ee`.

Corrected task SHA-256:
`6b4aabcde420907aa8c18d209d1d3d75a4053294b253e9f240b174597abd673c`.

Accepted blocker checkpoint:

`reports/phase4-hollow-knight/checkpoints/20260722-post-scene-submission-stall-125405-VERIFIED_BLOCKER_NOT_GOLDEN`

- checkpoint verification: `60/60 PASS`;
- manifest SHA-256:
  `1323035fbf4cf9bdebff3a1a24197bde4245ef9583c73aa9bb008c21678a204b`;
- `HEAD`: `85587ab23eae215b3a77d1813c193b55c7940e45`.

Run evidence:

`reports/phase4-hollow-knight/laneA-post-scene-passive-stack-20260722-143030`

## First Attempt: Preserved UNKNOWN

## Preflight

- Live HK/Wine/wineserver/ABZU collisions: `0`.
- Run contract: `READY`, blockers `[]`.
- Baseline typed-tree identity:
  `a0d78de3457ba291527b5816c947b1508a1292769ad2e82b48c183940ff2bd62`.
- Staged typed-tree identity:
  `9bb2d5f863ab6261a60b756adb98b11cdd1c02c062cdbaca58c2598a41871546`.
- Staged diff: exactly one regular file,
  `lib/wine/x86_64-windows/mono-profiler-hk_language.dll`.
- Staged and live-prefix actuator SHA-256:
  `7c62033f7315aaf14f31783c32b0ddb46a14757a3fb4e0e248c291c47b4a570b`.
- Environment names: `116/116`; missing/extra/forbidden names: `0/0/0`.
- Detached launch: stdin `DEVNULL`, no PTY, `pid=pgid=sid=9771`.
- Timeout requested: `2400s`.

The environment model incorrectly treated the accepted final-child
`WINEDLLPATH` as a parent-launch value. This error was not visible in the name
set or staged artifact checks; it became visible in the actual sealed
`final-child.json` before any product measurement was accepted.

## Fail-Closed Boundary

The only child value mismatch was `WINEDLLPATH`:

| Property | Expected child | Actual child |
|---|---:|---:|
| Environment fields | 116 | 116 |
| `WINEDLLPATH` entries | 5 | 6 |
| Unique entries | 5 | 5 |
| Value SHA-256 | `cce9f264e671dcf4d6cf22894a40879802eb6ba1b8b941d1b9a8a78f6cc74432` | `890a9a3da7130a947ca8bf490ff27966b8735d5312076d2b1db2608b0237a228` |

The bare DXMT overlay root appeared twice in the actual child. The remaining
four entries and their order were unchanged.

Root cause is deterministic: `scripts/mr-run.sh:405-410` prepends
`MACRUNNER_DXMT_ROOT` to an existing `WINEDLLPATH`. The launch environment was
built from the previously accepted five-entry **child** value, so the runner
prepended a second copy. This is an orchestration contract defect, not a new
Unity/product wall.

After detecting the drift, only runner PID `9771` was sent `TERM`; the runner's
scoped cleanup removed its prefix and Wine tree. No global process signal was
used. The product attempt was consumed and was not retried.

## Early Diagnostic Facts

These facts are preserved but are not admitted as a post-scene comparison:

| Measurement | Count |
|---|---:|
| `observer-init status=armed allocations=excluded` | 1 |
| `GetBuffer` | 1 |
| `Present` | 0 |
| `Present1` | 0 |
| `Performing automatic level start.` | 0 |
| `UNSUPPORTED` | 0 |
| `MEMORY_FAULT` | 0 |
| reject | 0 |
| SIGHUP | 0 |

The runner recorded exit `0` after scoped termination. This is not a nominal
2400-second completion and does not override the admission failure.

## Sampling Result

The automatic-level-start boundary was not reached. Therefore the authorized
`+30s` and `+120s` sampling points did not exist, and `sample` was not invoked.
Raw sample count: `0`.

No native wait/spin/deadlock class can be adjudicated from this attempt.
`STACK_CLASSIFIED_PASS` is not supported.

## Exact Correction Boundary

For a separately authorized run, construct the sealed **parent**
`WINEDLLPATH` with exactly four entries, preserving order:

1. DXMT `x86_64-windows`;
2. DXMT `x86_64-unix`;
3. staged runtime `x86_64-windows`;
4. staged runtime `x86_64-unix`.

Allow the unchanged runner at `scripts/mr-run.sh:405-410` to prepend the bare
DXMT root exactly once. The resulting child must then reproduce the accepted
five-entry value, with five unique entries and the sealed child value hash.
No runner/source change is required. This correction was not applied or run in
this task.

Only after exact child environment equality may the same `+30s/+120s` passive
sampling task be re-authorized. Shader work remains unauthorized.

## Cleanup And Evidence

- Disposable prefix: absent.
- Disposable staged clone: post-run typed-tree identity verified unchanged,
  then removed.
- Active HK/Wine/wineserver/ABZU residue: `0`.
- Translation cache preserved. Pre-run: 53021152 bytes,
  `2e5a3a174094cf88a1d650a8652692c3b2afda3891ac6e610b1b4e830a639715`;
  post-run: 53789332 bytes,
  `0306b24a96390bf48ce0459c88b71346274ab95746e41441eea9f3ffc800fea1`.
- Free disk after cleanup: `57 GiB`.

| Evidence | SHA-256 |
|---|---|
| `PRE-RUN-IDENTITY.json` | `dd361158ece537637b978931159d1dfecf7364dc866cfced5adf6cc3bff4ae19` |
| `EXPECTED-CHILD-ENV.json` | `2315355ac22cffd37f75a6af351acbb490204ac7a5df7bf87928f38afdb94494` |
| `STAGED-DIFF.json` | `af98c0e07a37ca9f9820c97d4abd23bfe2c1259252739296c48064925634efbf` |
| `run-contract.json` | `93b18cda4d1c7a82daea7481ecd10391ce6856b30302ebb6b2cd9cb59ea1331a` |
| `final-child.json` | `76f60c8a17cc3ab4bab7f4f4bb4e6b320050fac196eab027968d12374d91212a` |
| `ADMISSION-FAILURE.json` | `b6fa5eda1671bc9c61a473a3e1874d861a192f5ef24664d954f031e50e336ee7` |
| `POST-RUN-IDENTITY.json` | `6c8a781285b7dcb3737c394932c89145c4049c707dda5a83e2bc50e3471e9133` |
| `run.log` | `c09927be71268c7d9306cc3f6c1cac10d68946a0bea35dec5b68464771b776ae` |

Raw `final-child.json` contains environment values and remains local evidence
only. It must not be committed or uploaded. Historical Return artifact
causality remains `UNKNOWN`.

## Corrected Attempt: STACK_CLASSIFIED_PASS

Corrected run evidence:

`reports/phase4-hollow-knight/laneA-post-scene-passive-stack-corrected-20260722-145129`

The corrected parent environment contained four ordered, unique DLL roots.
The unchanged runner prepended the bare DXMT root once, producing the required
five-entry child value.

- Run contract: `READY`, blockers `[]`.
- Live collisions before launch: `0`.
- Actual child environment: `116/116`; missing/extra/forbidden/value-drift:
  `0/0/0/0`.
- Child `WINEDLLPATH`: `5/5` entries/unique entries; value SHA-256:
  `cce9f264e671dcf4d6cf22894a40879802eb6ba1b8b941d1b9a8a78f6cc74432`.
- Baseline/staged typed-tree identities:
  `a0d78de3457ba291527b5816c947b1508a1292769ad2e82b48c183940ff2bd62` /
  `9bb2d5f863ab6261a60b756adb98b11cdd1c02c062cdbaca58c2598a41871546`.
- Staged diff: exactly the sealed actuator PE, SHA-256
  `7c62033f7315aaf14f31783c32b0ddb46a14757a3fb4e0e248c291c47b4a570b`.
- Detached non-PTY runtime: runner PID `29746`, HK PID `38952`, timeout
  `2400s`.

`HEAD` changed from `85587ab2...` to `beb53d30...` after launch because the
coordinator committed documentation. The pinned corrected task, runner, sync
script, staged artifacts, and effective child environment remained unchanged;
this did not alter the running product inputs.

## Corrected Boundary

The exact marker occurred once at byte offset `469174..469207` and was detected
at elapsed `+1739s`. Counts through the marker were:

| Measurement | Count |
|---|---:|
| `GetBuffer` | 1 |
| `Present` | 48 |
| `Present1` | 48 |
| draw | 0 |
| encoder | 1 |
| `UNSUPPORTED` / `MEMORY_FAULT` / reject / SIGHUP | 0 / 0 / 0 / 0 |

From the marker through the final bounded timeout, post-boundary
`GetBuffer/Present/Present1/draw/encoder` remained `0/0/0/0/0`; faults,
rejects, and HUP remained zero. The submission stall therefore reproduced.
`mr-run` ended at its bound with recorded exit `124` and scoped prefix removal.

## Passive Samples

Exactly two `sample` calls were made. Both used HK PID `38952`, returned `0`,
and left that PID alive. Synchronous monitor work delayed the actual captures
past their target epochs; the actual offsets are retained below. No timing
retry or third sample was performed.

| Slot | Actual offset | Process | Threads | Sample bytes | Sample SHA-256 | Pixel |
|---|---:|---|---:|---:|---|---|
| `plus-030` | `+58s` | `R`, 184.8% CPU, 2966384 KiB RSS | 59 | 745400 | `63f9dd0450102553137818a64a6460727ae5fc135ce2e35c750451bed3ce01ec` | BLACK, 0 non-black, window 9738/PID 38952 |
| `plus-120` | `+140s` | `R`, 183.4% CPU, 2788576 KiB RSS | 59 | 726272 | `f31827798f790cd81eed87be984eb6c219ba286b0ddd1f48d697c404e97e70c8` | BLACK, 0 non-black, window 9738/PID 38952 |

The first compact thread-state command requested Darwin's unsupported `tid`
column and returned `1`; the raw sample already contained thread IDs. A
zero-safe `ps -M` fallback returned `0` and 59 rows without rerunning `sample`
or touching HK.

## Stack Classification

Both samples identify the same relevant native thread and the same stable
class: **HyperBridge/JIT spin or guest-PC loop**.

Thread `4704952` (`0x47cab8`) is also the native thread recorded for the
successful `CreateSwapChainForHwnd`. It stayed active in both samples:

```text
__wine_unix_call_dispatcher
macrunner_hb_x64_thread_entry+348
macrunner_hb_run_x64+35276
hb_jit_runtime_run+5396
generated host PC 0x11d397e38
hb_jit_helper_exec_two_block_loop+2904
hb_jit_helper_exec_ir_block_once+7644
exec_instr+14404
mem_read+188
hb_memory_read
find_region_normalized
```

The sample counts support stability: dispatcher/HB entry appeared in
`3685/3720` observations in the first sample and `3758/3789` in the second;
the generated host PC `0x11d397e38` and two-block helper corridor recur in
both.

Thread `4704947`, the macOS main thread, spent `3720/3720` and `3789/3789`
observations in the normal Cocoa path:

```text
main -> __wine_main -> CFRunLoopRun
  -> _CFRunLoopRunSpecificWithOptions -> __CFRunLoopRun
  -> __CFRunLoopServiceMachPort -> mach_msg -> mach_msg2_trap
```

This refutes `_MetalLayer_setProps` / main-queue `dispatch_sync` as the sampled
boundary and refutes Cocoa-only operation with a missing Unity producer. The
relevant producer is active rather than sampled in a Wine synchronization
wait. The samples do not yet identify a guest RIP, IR opcode, or semantic root
cause, so none is claimed.

## Exact Next Fix Boundary

Map generated host PC `0x11d397e38` on Thread `4704952` through
`hb_jit_helper_exec_two_block_loop` to its guest RIP and the two contained IR
blocks. Then identify the stable loop/memory-read condition before changing
code. Shader work remains unauthorized because submission stops before a
post-scene Present.

## Corrected Cleanup And Evidence

- Post-run staged tree: `4732/4732`, identity unchanged at
  `9bb2d5f863ab6261a60b756adb98b11cdd1c02c062cdbaca58c2598a41871546`;
  verified before scoped removal.
- Disposable prefix and staged clone: absent.
- Post-run HK/Wine/ABZU collision count: `0`.
- Translation cache preserved. Pre-run: 53789332 bytes,
  `0306b24a96390bf48ce0459c88b71346274ab95746e41441eea9f3ffc800fea1`;
  post-run: 58285128 bytes,
  `ef8d5deb90403272b297107e7f8420c1be2520c57909045be8c0892b1119fd63`.
- Classification remains `NOT_GOLDEN`; no commit or snapshot was created.

| Corrected evidence | SHA-256 |
|---|---|
| `PRE-RUN-IDENTITY.json` | `d821c1b4fb9b0fe59b13cc5cb5f865654405ac00e0fe1fb54bbbdf27a9cf83b2` |
| `EXPECTED-CHILD-ENV.json` | `86aae97c7c4abf7383bb4ea7c9a0944944f8529f9047587b7f8de8fbf2cc10ed` |
| `STAGED-DIFF.json` | `af98c0e07a37ca9f9820c97d4abd23bfe2c1259252739296c48064925634efbf` |
| `run-contract.json` | `d972170ec60d8fa14aea0d0b713ddb97d917c78e5dd5a4d7cecae6d1f5340efa` |
| `final-child.json` | `f5ad555dddfaaf72e61b841a622b44f803e2518da9c5a2fb9a7381f727c416c6` |
| `LIVE-ADMISSION.json` | `9eb2f79d03c99df964e312ea8dfce986c1dabba744741dfb4fc973fa54e5b5fa` |
| `automatic-level-start-boundary.json` | `7e900ae89c4cc4de0415a9ffdb5f402cff86bacd53733eca04ef8741a4e9a724` |
| `run.log` | `39f7dcb7c8eb621f2c30ef7c69ce3effd46dac5fac51d03a495e3c9d862dd895` |
| `POST-RUN-IDENTITY.json` | `c6f75b8b00a803e459a5a29963aa9ff49c00bff48943023ccfc736024e213e40` |
| `SAMPLE-ADJUDICATION.json` | `eff6cf9e7d168513f781d13e37da75735c65f618fe97d7a2f3e403e6866c0e05` |

Raw `final-child.json` remains local-only evidence because it contains
environment values. Historical Return artifact causality remains `UNKNOWN`.
