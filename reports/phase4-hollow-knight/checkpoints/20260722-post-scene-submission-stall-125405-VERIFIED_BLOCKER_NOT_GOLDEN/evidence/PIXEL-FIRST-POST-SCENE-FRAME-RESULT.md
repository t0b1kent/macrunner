# Pixel-First First Post-Scene Frame Result

Classification: `NOT_GOLDEN`

Machine verdict: `POST_SCENE_SUBMISSION_STALL`

## Scope

Three separately authorized runtime attempts are preserved. The first attempt
was the original post-scene task; the second was explicitly authorized by the
corrected task after the first orchestration failure; the third used the
staged-deploy task that removed the remaining deploy race. There was no retry
within any authorization, source change, rebuild, input, focus/activation
change, scene forcing, shader override, production install, or GOLDEN action.

Run evidence:

`reports/phase4-hollow-knight/laneA-post-scene-first-frame-20260722-120156`

The existing capability checkpoint remains unchanged:

`reports/phase4-hollow-knight/checkpoints/20260722-oracle-managed-actuator-105203-VERIFIED_CAPABILITY_NOT_GOLDEN`

## Preflight And Deployment

- Task SHA-256: `3f21440d0f1a56b5dca188424c5698b1b931a30b2fb9246fd35f2f3aa4877565`.
- `HEAD`: `78e1b3c4381104559ee3d7fa8e3cb23b889f1283`.
- Present checkpoint verification: `4742/4742`, zero mismatch/missing.
- Capability checkpoint `SHA256SUMS`: PASS.
- `scripts/mr-run.sh`: `4b23ba831e926d5475b5e13ea22846ad7c73b3b8b926264d5a88ae854a4213a9`.
- Actuator PE: regular file, 34304 bytes, SHA-256
  `7c62033f7315aaf14f31783c32b0ddb46a14757a3fb4e0e248c291c47b4a570b`.
- Preflight collisions: `0`.
- Run contract: `READY`, blockers `[]`.
- Actuator replacement occurred after prefix sync/overlay and before
  `final-child.json` or `wine-child.pid`; deployed SHA matched the accepted PE.
- Runtime timeout requested: `2700s`.

## Runtime Facts Before The Boundary

These are absolute facts only; they do not override the final `UNKNOWN` verdict.

| Measurement | Count |
|---|---:|
| `CreateSwapChainForHwnd rc=0x0` | 2 |
| `GetBuffer` | 1 |
| `Present` | 48 |
| `Present1` | 48 |
| `observer-init status=armed allocations=excluded` | 1 |
| exact-one lookup | 1 |
| `SetLanguage("EN") return status=ok` | 1 |
| `ConfirmLanguage() return status=ok` | 1 |
| retained process-lifetime handle | 1 |
| GameManager manager-created | 1 |
| UIManager manager-created | 1 |
| GameCameras manager-created | 1 |
| `Performing automatic level start.` | 1 |
| `UNSUPPORTED` | 0 |
| `MEMORY_FAULT` | 0 |
| reject | 0 |

The exact manager and boundary anchors are at `run.log` lines 1991, 1996, 1997,
and 1999. The automatic-level-start boundary was detected at epoch
`1784687635`, `1640s` after run start. The byte-exact prefix through the marker
is `boundary-prefix.log` (503430 bytes).

## Fail-Closed Reasons

### 1. Post-scene measurement was interrupted

The run-local counter used a `grep | wc` pipeline under `set -euo pipefail`.
Immediately after writing `boundary-prefix.log`, it evaluated the first zero
counter, `method=Draw`. `grep` returned 1, the monitor shell exited, and its PTY
sent SIGHUP to the still-running `mr-run` job and HK child. The two `Hangup: 1`
records are at `run.log` lines 2000-2001.

Only 408 log bytes exist after the marker. They contain the SIGHUP termination
and zero `GetBuffer`, `Present`, `Present1`, `UNSUPPORTED`, `MEMORY_FAULT`, or
reject records. That near-zero interrupted interval is not a valid observation
of post-scene submission. The required +10/+30/+60/+180/+300 captures do not
exist, and the process did not remain alive for 300 seconds after the boundary.

Therefore neither `POST_SCENE_SUBMISSION_STALL` nor `POST_SCENE_RENDER_BLACK`
is supported.

### 2. Full effective environment drifted

The accepted child environment has 116 fields. This run's child has 129. It
added the following 13 names:

`API_KEY_DEEP`, `API_TOKEN`, `BROWSER_USE_AVAILABLE_BACKENDS`, `CODEX_CI`,
`CODEX_THREAD_ID`, `GH_PAGER`, `GIT_PAGER`, `LC_ALL`,
`NODE_REPL_TRUSTED_BROWSER_CLIENT_SHA256S`, `NODE_REPL_TRUSTED_CODE_PATHS`,
`OPENAI_API_KEY`, `PAGER`, and `SSH_AUTH_SOCK`.

`SHLVL` also changed outside the allowed identity-path delta. Values are not
reproduced here. The semantic HK variables remained correct, and `WINEDLLPATH`
had five unique entries, but the task explicitly makes any identity drift
`UNKNOWN`.

Raw `final-child.json` contains environment values and must not be committed or
uploaded.

## Cleanup And Integrity

- Disposable prefix: absent after scoped `mr-run` cleanup.
- Active HK/Wine/wineserver/ABZU residue: `0`.
- Translation cache was preserved at its existing root.
- Cache before run: size 42647644, SHA-256
  `a826da4083068182061ff9d360df3f9d27df4606b10ac5008e238be3dfeabf46`.
- Cache after run: size 46327800, SHA-256
  `78dbab90bd4a24441e6ea8e10a4f0bb7867a80aabb15c60087595081ac58c0b9`.
- Free disk after cleanup: `60 GiB`.

Evidence identity:

| File | SHA-256 |
|---|---|
| `PRE-RUN-IDENTITY.json` | `8c242a146ee00c9fb4fc81ec5e6273234d6487e98af515e6559b634ca3af49e5` |
| `actuator-deploy.json` | `d43fe1434231af803cb203b74922a9fcb83217672a7f28a26d826e5339e9cac8` |
| `run-contract.json` | `d6bb1845473b444fc7c5f4a0e12eb85009d6c1873a5458f0e98d5e7f435e50e5` |
| `final-child.json` | `a03ddd045bbe2bff82d7faf152d60dec5b2e28be7c0552ca50bd90c783f3e216` |
| `run.log` | `e284c7dd12c81b4dd9799df348cca2acc7a058645af490a0ff6f664ae2024f05` |

## Corrected Attempt

Corrected task SHA-256:
`4fab6d1afe65b1be9b8a84ecaeaa23ab691006c8044d5dab047d8978c92848f8`.

Corrected run evidence:

`reports/phase4-hollow-knight/laneA-post-scene-first-frame-corrected-20260722-124351`

### Corrections Proven

- The prelaunch environment was constructed from an empty mapping and the
  accepted environment record, not inherited from the terminal.
- Preflight and actual `final-child.json` both contain exactly 116 names.
- Missing, unexpected, and explicitly forbidden names: `0/0/0`.
- Environment names SHA-256:
  `9793680dfb7541b522811065ad2f1a6693cb0b6d64147366288bfbe6014ef532`.
- Actual values match the expected baseline plus the seven approved identity
  substitutions; unapproved value changes: `0`.
- Actual `SHLVL=5`; `WINEDLLPATH` contains five unique entries.
- All eight zero-safe counters returned integer zero and rc=0 on an empty
  synthetic log under `set -euo pipefail`.
- `mr-run` was detached with stdin `DEVNULL`, no PTY, and a new session/process
  group (`pid=pgid=sid=3917`). Monitor termination could not send it SIGHUP.

### Corrected Attempt Failure

The first detached deploy-monitor invocation exited before polling because its
embedded hash-extraction expression expanded an unset positional parameter
under `set -u`. The detached runtime remained alive and received no HUP.

When the corrected monitor observed prefix sync, `final-child.json` and
`wine-child.pid` already existed. It therefore failed closed and did not replace
the file. The prefix target remained the managed baseline PE:

- observed SHA-256:
  `ca5a6e2259fe3875c58eebdebc0ccdae922c5621e6782f84ae11870fb111bb69`;
- required actuator SHA-256:
  `7c62033f7315aaf14f31783c32b0ddb46a14757a3fb4e0e248c291c47b4a570b`.

The scoped process group was terminated immediately, followed by a scoped
prefix wineserver cleanup. `mr-run` recorded exit 143 and removed the prefix.
No observer, managed call, swapchain, GetBuffer, Present, manager, automatic
level start, opcode fault, memory fault, reject, or HUP was recorded. These are
only early-stop facts; the attempt cannot make a product claim.

Required post-scene measurements and all five captures are absent. Per the
corrected no-retry contract, no additional runtime was started.

Corrected evidence identity:

| File | SHA-256 |
|---|---|
| `PRE-RUN-IDENTITY.json` | `126ec717f5f40f46563107bb89ac7bc3881b061ebc982d24e4b1d0ea20c62707` |
| `detached-launch.json` | `8b63cd41f47be3b4ff1ae0068e2bb14a57b57f3797b5a5a66c1bb7b17afbaf1d` |
| `actuator-deploy-failure.json` | `daa7001d3dd49db8487958fcfad8985ad169f0f321f681732d88e81510cf4545` |
| `run-contract.json` | `beb7be8058ca93c5ab88d5a8eb7cc4c63645288af1279b85bf1c22067c173d60` |
| `final-child.json` | `98a04aca8f80468674b3212d2eccc35fe11571f0689b4f88af88d0cfcd00c470` |
| `run.log` | `c8a2e1eb2043d7d38cecdab5ec396fc1d336d1cac839f55c1f8f7b1b311ece46` |

The shared translation cache was preserved. Corrected-attempt pre-state was
46327800 bytes / `78dbab90bd4a24441e6ea8e10a4f0bb7867a80aabb15c60087595081ac58c0b9`;
post-state is 48734884 bytes /
`03d85117a0292d1be2f90f7a4daddbe21084b7a12a3043adfb5caf4779a15d49`.
The disposable prefix is absent, HK/Wine/wineserver/ABZU residue is zero, and
free disk is `60 GiB`.

Raw corrected `final-child.json` also remains local evidence only; its values
must not be committed or uploaded.

## Staged-Deploy Attempt

Staged-deploy task SHA-256:
`d83824ff0adcc22ee4550271212eadfe1b44efe4aa89f774e0b0241afdfe3007`.

Run evidence:

`reports/phase4-hollow-knight/laneA-post-scene-first-frame-staged-20260722-125405`

### Admission And Deployment

- `HEAD`: `78e1b3c4381104559ee3d7fa8e3cb23b889f1283`.
- Preflight collisions: `0`; run contract: `READY`, blockers `[]`.
- Accepted child environment: `116/116` names; missing, unexpected, and
  forbidden names: `0/0/0`; unapproved value changes: `0`.
- Environment names SHA-256:
  `9793680dfb7541b522811065ad2f1a6693cb0b6d64147366288bfbe6014ef532`.
- `WINEDLLPATH`: five entries, five unique roots; original checkpoint-dist
  references in the child environment: `0`.
- Baseline typed-tree identity:
  `a0d78de3457ba291527b5816c947b1508a1292769ad2e82b48c183940ff2bd62`.
- Staged typed-tree identity:
  `9bb2d5f863ab6261a60b756adb98b11cdd1c02c062cdbaca58c2598a41871546`.
- The staged tree differed by exactly one regular file:
  `lib/wine/x86_64-windows/mono-profiler-hk_language.dll`.
- Staged and live-prefix actuator SHA-256:
  `7c62033f7315aaf14f31783c32b0ddb46a14757a3fb4e0e248c291c47b4a570b`.
- The live file came from normal prefix sync. No post-launch deployment or
  deploy monitor ran. `observer-init status=armed allocations=excluded` was
  present exactly once.
- Detached lifecycle: stdin `DEVNULL`, no PTY, `pid=pgid=sid=27528`.

The staged tree was rehashed after runtime and before scoped removal. Its
identity remained `9bb2d5f...1546`, with `4732` typed entries and diff count
`0` against the sealed pre-run staged manifest.

### Managed And Pre-Boundary Facts

| Measurement | Count |
|---|---:|
| `CreateSwapChainForHwnd rc=0x0` records | 2 |
| `GetBuffer` | 1 |
| `Present` | 48 |
| `Present1` | 48 |
| exact-one lookup | 1 |
| `SetLanguage("EN") return status=ok` | 1 |
| `ConfirmLanguage() return status=ok` | 1 |
| retained process-lifetime handle | 1 |
| GameManager manager-created | 1 |
| UIManager manager-created | 1 |
| GameCameras manager-created | 1 |
| `Performing automatic level start.` | 1 |
| `UNSUPPORTED` | 0 |
| `MEMORY_FAULT` | 0 |
| reject | 0 |
| SIGHUP | 0 |

The exact marker begins at byte `467475` and ends at byte `467508`. The
byte-exact prefix through the marker has SHA-256
`01144dc0951e90f17dd1c8b6f56e53990f89f4e7ff3ee97364a3461e0752a0fd`.
The boundary was first observed by the passive poll at about run `+1645s`; the
immutable boundary manifest was written at epoch `1784690846` (`+1677s`).

### Post-Scene Measurement

The same runner and HK window process were alive through the full measurement.
The sealed cutoff was taken `352s` after boundary-manifest detection, so it
covers more than the required 300-second interval after the earlier exact log
marker.

| Strictly after marker | Count |
|---|---:|
| `GetBuffer` | 0 |
| `Present` | 0 |
| `Present1` | 0 |
| draw | 0 |
| encoder | 0 |
| `UNSUPPORTED` | 0 |
| `MEMORY_FAULT` | 0 |
| reject | 0 |
| SIGHUP | 0 |

All zero-count operations completed normally. `post-scene-300s.log` is the
byte-exact post-marker slice at cutoff; its SHA-256 is
`3cb373a40f0ad6a612f71f7575e76eaad3b639e393d1252fc44d03407c888424`.

| Requested capture | Actual detection delta | Pixel gate |
|---|---:|---|
| `+10s` | `+20s` | `BLACK` |
| `+30s` | `+39s` | `BLACK` |
| `+60s` | `+60s` | `BLACK` |
| `+180s` | `+180s` | `BLACK` |
| `+300s` | `+300s` | `BLACK` |

The first two captures were late by 10s and 9s because boundary evidence was
sealed synchronously before capture. Their actual trigger epochs are retained,
not relabelled. This does not weaken the submission-stall measurement: the
strict post-marker counter slice covers 352 seconds, and no Present occurred at
any point in it. All five captures resolved the same live HK process/window and
reported `0` non-black and `0` colorful pixels.

The run then reached its normal hard cap and recorded `[mr-run] exit=124`.

### Cleanup And Identity

- Disposable prefix: absent after scoped runner cleanup.
- Disposable staged-dist clone: typed-tree verified, then removed.
- Active HK/Wine/wineserver/ABZU residue: `0`.
- Translation cache was preserved. Pre-run: 48734884 bytes,
  `03d85117a0292d1be2f90f7a4daddbe21084b7a12a3043adfb5caf4779a15d49`;
  post-run: 53021152 bytes,
  `2e5a3a174094cf88a1d650a8652692c3b2afda3891ac6e610b1b4e830a639715`.
- Free disk after cleanup: `60 GiB` (`df -h`).

Staged-deploy evidence identity:

| File | SHA-256 |
|---|---|
| `PRE-RUN-IDENTITY.json` | `ef2512ddaba5c2a10e3645aec0b5d2ba55bc48a570c1dde2829b1e7fd6b3707c` |
| `STAGED-DIFF.json` | `2b4ae8bd02cecf9d31802b054154e905f370221845eb7a44d23a7683913ce32f` |
| `LIVE-DEPLOYMENT.json` | `577577e6e7f301ce6d39ebd01ef14fdc8b8da28c8e800c88a7620207811226d6` |
| `run-contract.json` | `302733883329a562c701d80fd931023acf4b38eacc24c82c76cfd4a90857fa6a` |
| `final-child.json` | `9fcbaf039e3c2aeee9548f1fbe90e6b38783e5cc26385b0fcf948a60334f255c` |
| `automatic-level-start-boundary.json` | `f9e3c4524d75335dce8f5413f61ba83657b9bea7df6f3094defc95fb5eebbcd3` |
| `POST-SCENE-300S-SUMMARY.json` | `c40feefa45731679a577d2fdf9544b892e5811e1692a8d78e596193bbeb04f5d` |
| `POST-RUN-IDENTITY.json` | `3b8501453ef218425edce82367bc3693eaee18e06ef70ff50afebaa242b623d2` |
| `run.log` | `1642b263817d1a4f51ae395618117f7becef8c246e636476756b7d6b9fb1d3c5` |

Raw staged-deploy `final-child.json` remains local evidence only; its values
must not be committed or uploaded.

## Verdict

`POST_SCENE_SUBMISSION_STALL`.

The third attempt removed both prior orchestration defects, reached the exact
automatic-level-start boundary, and observed the same process for more than 300
seconds afterward. No post-boundary `GetBuffer`, `Present`, `Present1`, draw, or
encoder record occurred. The five passive frames were black, but shader-value
work is not authorized because post-scene Present was zero.

No snapshot or commit was created. The first two attempts remain preserved as
`UNKNOWN` orchestration evidence; they are not used for the product verdict.
Historical Return artifact causality remains `UNKNOWN`.
