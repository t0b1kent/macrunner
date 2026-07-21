# Pixel-First Winemac One-File Binary Bisect

Classification: **NOT_GOLDEN**

Machine verdict: **UNKNOWN**

Reason code: **ENVIRONMENT_DRIFT**

Absolute runtime result: the arm using `winemac.so` SHA-256
`02b4d94ecb5827ea331decccb846f1bf6ff97df632ea28f6b40951794f8c9604`
reached `CreateSwapChainForHwnd rc=0`, `GetBuffer=1`, `Present=448`, and
`Present1=448`. This proves that `02b4` does not categorically prevent render
submission in this arm.

The strict one-file causal verdict is nevertheless `UNKNOWN`: the effective
first Wine child had one additional duplicate DXMT-root component in
`WINEDLLPATH` beyond the sealed identity substitutions. The run therefore did
not retain exactly one byte-level independent variable. No retry was made.

## Scope

- Accepted baseline, not rerun:
  `reports/phase4-hollow-knight/laneA-native-ntdll-bisect-B-corrected-20260722-try1-052828`
- Baseline result: `CreateSwapChainForHwnd rc=0`, `GetBuffer=1`, `Present=745`,
  `Present1=745`, faults/rejects `0`.
- New arm:
  `reports/phase4-hollow-knight/laneA-winemac-bisect-02b4-20260722-try1-064325`
- Disposable staging:
  `reports/phase4-hollow-knight/winemac-bisect-20260722-NOT_GOLDEN/arm-W`
- Exactly one runtime was launched. No source edit, rebuild, production
  install, input, focus, activation, retry, commit, or GOLDEN promotion.

## Artifact Identity

The staged dist was cloned from the accepted corrected-B dist. Typed inventories
were computed with symlinks recorded by target and never followed.

- Baseline entries: `4732`
- Test entries: `4732`
- Different entries: exactly `1`
- Different entry type: regular file
- Path: `lib/wine/aarch64-unix/winemac.so`
- Baseline: `631592` bytes,
  `65bf6bf9b9d3e1030e1331a3b59b5c56282885fa7ac5469a617b26ff8546d0bc`
- Test: `631656` bytes,
  `02b4d94ecb5827ea331decccb846f1bf6ff97df632ea28f6b40951794f8c9604`

Unchanged selected native files:

- `aarch64-unix/ntdll.so`:
  `9a3f20b426adeeb86d70c06d0b7cfc9e687838e6eace5e43c282a19eb0363fc7`
- `aarch64-windows/ntdll.dll`:
  `3863660ac0b5f57c106d610d6a88d897a89a6c0ab626f890e66b4d572f02de4d`
- `x86_64-windows/ntdll.dll`:
  `b08516ba28c2d0d884b22673301bfc6eb3b5b48ee7458cd1248303289e431b78`

The post-run typed comparison still showed only the same `winemac.so` entry.

## Cache Identity

Preserved source before cloning:

- Path: `artifacts/hb-translation-cache/ntdll-9a3f20b426adeeb8-unity-movnt`
- `translation-cache.bin`: `22606624` bytes
- File SHA-256:
  `f5faa1dc60cb7a53e7466a1d5b31ad8d3ac0115a049b0bcc37deb48e8f0a7cfa`
- Typed inventory SHA-256:
  `26aced9e8d430ee129839cae182bac3cbb0279aac70953025b08558de590869e`

The fresh clone matched those values before launch. The preserved source still
matches after the run. Expected writes were confined to the per-arm clone;
post-run it is `34558724` bytes with SHA-256
`adb85acb077a129931e92de318e25f161f83b965433618adc147587c154f8a8e`.

## Effective Environment

- Imported environment fields: `113`
- Effective child fields: `113`
- Name sets: identical
- C0 control: enabled
- DXMT swapchain trace: enabled
- Mono profiler variables: absent
- `actxprxy`, `crt_case_fusion`, `wwise_observer`: present and unchanged
- Input/focus/activation variables and records: absent/zero
- Run contract: `READY`, blockers `0`, requested timeout `1200`

The expected differences were the new run, prefix, dist, cache, flight, log,
loader-path identities, `_`, and non-semantic `SHLVL` shell identity.

The first fail-closed boundary was `WINEDLLPATH`. The imported corrected-B
final-child value already contained the DXMT root. During launch,
`scripts/mr-run.sh:405-408` prepended `MACRUNNER_DXMT_ROOT` again. Compared with
the path-substituted baseline, the effective value had one additional identical
DXMT-root component. All intended components remained present and no new path
was introduced, but byte-exact environment parity was lost. The task's
`env drift => UNKNOWN` rule therefore takes precedence over the positive
Present counters.

## Runtime Result

- `mr-run` rc: `124`
- `run.log` contains one `[mr-run] exit=124` cleanup record
- `CreateSwapChainForHwnd rc=0`: observed
- `GetBuffer`: `1`
- `Present`: `448`
- `Present1`: `448`
- `UNSUPPORTED`: `0`
- `MEMORY_FAULT`: `0`
- Reject: `0`
- Focus milestone: `0`
- Input: `0`
- Activation: `0`
- Prefix after scoped cleanup: absent
- Hollow Knight/Wine/wineserver processes after cleanup: `0`

The recorded start was `2026-07-21T20:50:18Z`; the recorded end was
`2026-07-21T22:09:40Z`, a wall interval of `4762s`. The inner runner still
reported its configured timeout rc `124`, and both capture monitors completed
before cleanup. The cause of the wall-time discontinuity was not proven and is
not normalized away; it is an additional reason this arm remains NOT_GOLDEN.

## Pixel Evidence

First-Present capture:

- Timestamp: `2026-07-21T22:05:40Z`
- Verdict: `BLACK`
- `non_black_px=0`, `colorful_px=0`
- Capture rc: `3`
- Result SHA-256:
  `4af549aaffaef092555cd9a39a78ac8fdbe68396e6f4c4042359cef1e957e74c`
- Capture inventory SHA-256:
  `421df59192345089dc5fc94d880b11039e9617aa19ce95d25c02da9830e2ec82`

Bounded late capture:

- Scheduled at active monitor step `+1100s`
- Timestamp: `2026-07-21T22:07:35Z`
- Verdict: `BLACK`
- `non_black_px=0`, `colorful_px=0`
- Capture rc: `3`
- Result SHA-256:
  `6d58ccb52df5b4cd0a33b6e48839424968c81965e54c9b68c6645f8f0efea643`
- Capture inventory SHA-256:
  `edc92113235ea6a1fc612b03b813ac0acb26e085d1d7f73dd97a2645c2396784`

No non-black pixel was observed and no verified pixel snapshot was created.

## Evidence Hashes

- `PRE-RUN-IDENTITY.json`:
  `36bb4aae5caa9bd4814ebbc12a3a9b99494e01c09f97f8eea0d44b26ca95ff66`
- `run-contract.json`:
  `7fe3156b5238f453b3032207c34be27498f2252e57f9df3f4e87aa399bc87596`
- `final-child.json`:
  `1f7b257c023e42a161f70931ade522446200eb5d16a29ab912d56c7002d8954b`
- `run.log`:
  `409940c0983d32a9641782575d195cf228268abeb2d438a48b42128cd772f461`
- `flight.jsonl`:
  `7609921a5687cbb1a699a8231e397d398c04f93963f71c9585ae03feabc488c8`

`final-child.json` contains captured environment values and must not be
committed or uploaded.

## Decision

`WINEMAC_02B4_COMPONENT_CAUSAL` is refuted by the absolute runtime facts:
`GetBuffer`, `Present`, and `Present1` all occurred.

`WINEMAC_02B4_EXONERATED` is not admitted as a strict one-file A/B conclusion
because effective `WINEDLLPATH` drifted and wall timing was discontinuous.

Final machine verdict: **UNKNOWN / ENVIRONMENT_DRIFT / NOT_GOLDEN**.

No retry is authorized or performed. A future separately authorized repeat
would need to remove one inherited DXMT-root entry before `mr-run.sh` performs
its deterministic prepend, and must seal the resulting effective child value
before claiming one-file parity.
