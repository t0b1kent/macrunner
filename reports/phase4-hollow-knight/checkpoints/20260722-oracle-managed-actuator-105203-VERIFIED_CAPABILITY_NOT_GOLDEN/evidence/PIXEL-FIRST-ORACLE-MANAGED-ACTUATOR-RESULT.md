# Pixel-First Oracle Managed Actuator Result

Classification: `NOT_GOLDEN`

Machine verdict: `SCENE_CAPABILITY_PASS_RENDER_BLACK`

## Scope

Executed one bounded Hollow Knight runtime from the Present-positive checkpoint
plus Windows-oracle state, adding only the sealed no-allocation managed actuator.
No source edits, rebuild, retry, production install, input, focus change,
activation manipulation, field forcing, scene forcing, or semantic graphics
override was performed.

Run evidence root:

`reports/phase4-hollow-knight/laneA-oracle-managed-actuator-20260722-try1-105203`

Capability snapshot:

`reports/phase4-hollow-knight/checkpoints/20260722-oracle-managed-actuator-105203-VERIFIED_CAPABILITY_NOT_GOLDEN`

## Preflight

- Task file SHA-256:
  `6fa9629f4c8b4e41611f4241b957efd1261398b3721b07219e27a5975aca6434`.
- Checkpoint payload verification: `PASS 4742/4742`.
- Checkpoint `PAYLOAD-SHA256SUMS` SHA-256:
  `c6843d3eb61eb1d7933a4a8f34d1c607de6fcdb0892ed4504bbe7261cf485ac6`.
- `scripts/mr-run.sh` SHA-256:
  `4b23ba831e926d5475b5e13ea22846ad7c73b3b8b926264d5a88ae854a4213a9`.
- `scripts/sync-prefix-from-dist.sh` SHA-256:
  `492d392db11f104ef41bd533c8f6f7b8fd869eb2606a362889caee4adab8fb3a`.
- Actuator PE: regular file, `34304` bytes, SHA-256
  `7c62033f7315aaf14f31783c32b0ddb46a14757a3fb4e0e248c291c47b4a570b`.
- Parent env: `116` entries. Source oracle effective env was preserved with
  fresh identity paths and four actuator deltas:
  `MONO_ENV_OPTIONS`, `MACRUNNER_HB_LANGUAGE_FLOW_OBSERVER`,
  `MACRUNNER_HB_LANGUAGE_ONESHOT_SEQUENCE`, `WINEDLLOVERRIDES`.
- Preflight process collisions: `0`.
- Cache pre-run SHA-256:
  `22c8ff0f6cd791bfd32e02c945756a2ce2c1bd1ba66ed974cef492e7db94c048`.

## Deployment

The actuator was atomically installed into the disposable prefix after the
normal prefix sync and before `final-child`/Wine child evidence existed.

| Field | Value |
|---|---|
| Deploy status | `PASS` |
| Target path | `artifacts/_mr-run-aa-hk-oracle-managed-actuator-20260722-try1-105203/drive_c/windows/system32/mono-profiler-hk_language.dll` |
| Target type/size | regular / `34304` |
| Target SHA-256 | `7c62033f7315aaf14f31783c32b0ddb46a14757a3fb4e0e248c291c47b4a570b` |
| Sync lines | `1` |
| Later sync | `0` additional sync lines |
| `final-child` before deploy | `false` |
| Wine child PID before deploy | `false` |

## Runtime

| Measurement | Result |
|---|---|
| `mr-run` rc | `124` |
| `run-contract.json` | `READY`, `blockers=[]` |
| `run-contract.json` SHA-256 | `28c0b3076ed3d3b908fc73ca6ca869d8b28c8e978e67590d59dcc0196f656a3e` |
| `final-child.json` SHA-256 | `03e1321b03fac8a47806392b41a8c86488d756cc1e9a03de7e8177ae9effb1c9` |
| Effective env entries | `116` |
| Effective actuator env | `MONO_ENV_OPTIONS=--profile=hk_language`; language observer `1`; oneshot `1` |
| Effective `WINEDLLOVERRIDES` | `mono-profiler-hk_language=n;d3d9=n,b;d3d11,dxgi,d3d10core,winemetal=n,b` |
| `WINEDLLPATH` | `5` entries, `5` unique |
| Profiler init | `observer-init status=armed allocations=excluded` |
| `init-failed` | `0` |
| UNSUPPORTED / MEMORY_FAULT / reject | `0 / 0 / 0` |
| Current-run `Player.log` | present, size `0`, SHA-256 `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |

`run.log` SHA-256:

`ce2d2607538ee596109f0cb25a093fb16b051a4dabcac49d516ed9713c1b4f8e`

## Managed Sequence

The no-allocation actuator executed the intended sequence exactly once.

| Marker | Count |
|---|---:|
| `HighlightDefault` / `unity-selection` | `2` / `2` |
| `oneshot-lookup exact_one=1` | `1` |
| `SetLanguage("EN")` begin / ok | `1` / `1` |
| `ConfirmLanguage()` begin / ok | `1` / `1` |
| retained process-lifetime gchandle | `1` |
| `allow-scene-activation` enter/return pairs | `2` |
| `GameManager Awake` | `1` |
| `UIManager Awake` | `1` |
| `GameCameras Awake` | `1` |
| `Menu_Title` | `0` |
| `Opening_Sequence` | `0` |
| scene load markers | `0` |

Exact capability sequence reached:

```text
observer-init status=armed allocations=excluded
oneshot-lookup exact_one=1
SetLanguage("EN") return status=ok
ConfirmLanguage() return status=ok
retained_handle=<process lifetime>
manager-created class=GameManager method=Awake
manager-created class=UIManager method=Awake
manager-created class=GameCameras method=Awake
```

## Graphics And Pixels

| Measurement | Result |
|---|---:|
| CreateSwapChainForHwnd rc=0x0 | `2` |
| GetBuffer | `1` |
| Present | `48` |
| Present1 | `48` |

All passive captures selected the Hollow Knight window and stayed black.

| Capture | Gate rc | Verdict | non_black_px | PNG SHA-256 | Gate JSON SHA-256 |
|---|---:|---|---:|---|---|
| first-present | `3` | `BLACK` | `0` | `63373382cfa36078355ba24feb69e5e94474e40ec28f26bb01c8bfd7f4ed18e6` | `d1f3d10325fb99fe7ad093e18812e204f970a9f6d783f2877f5238f85c7e9272` |
| pre-managed-sequence | `3` | `BLACK` | `0` | `63373382cfa36078355ba24feb69e5e94474e40ec28f26bb01c8bfd7f4ed18e6` | `ef9d6ff405f62570158a92c3a422d895a64867f87547dea36918802d2540b6df` |
| post-confirm-plus10 | `3` | `BLACK` | `0` | `63373382cfa36078355ba24feb69e5e94474e40ec28f26bb01c8bfd7f4ed18e6` | `bb7bc24e9b96cbb317fbc3048471ea789af96de2f6b1d403267a53be2bc62b07` |
| post-confirm-plus60 | `3` | `BLACK` | `0` | `63373382cfa36078355ba24feb69e5e94474e40ec28f26bb01c8bfd7f4ed18e6` | `62a5b5339d2641a9dd29046728710b97b855a909dd5240e526c865e1e5f12ebd` |
| post-confirm-plus240 | `3` | `BLACK` | `0` | `63373382cfa36078355ba24feb69e5e94474e40ec28f26bb01c8bfd7f4ed18e6` | `65873427aa3b0d8db3af8de63c2b47727905ecde6c1052d2958f47cd5638c330` |

## Cleanup And Integrity

- Scoped disposable prefix after cleanup: `ABSENT`.
- Active HK/Wine/wineserver/ABZU residue after cleanup: `0`.
- Cache post-run SHA-256:
  `a826da4083068182061ff9d360df3f9d27df4606b10ac5008e238be3dfeabf46`.
- Free disk after cleanup: `60 GiB`.
- Raw `final-child.json` is evidence only and is not copied into the compact
  snapshot because it contains environment values.
- Full `run.log` and translation-cache bytes are not copied into the compact
  snapshot.

## Verdict

`SCENE_CAPABILITY_PASS_RENDER_BLACK`.

The managed actuator carried the Windows-oracle baseline through the language
sequence and into manager creation with live Present/Present1 and zero
opcode/fault/reject wall. The rendered window remains fully black at every
required capture point, so this is a verified capability milestone, not a pixel
pass. Historical Return artifact causality remains `UNKNOWN`, and this result
returns the next task to shader value production.
