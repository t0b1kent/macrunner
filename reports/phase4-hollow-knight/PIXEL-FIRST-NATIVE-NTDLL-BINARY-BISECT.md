# Pixel-First Native NTDLL Binary Bisect

Classification: NOT_GOLDEN

Verdict: NTDLL_PAIR_EXONERATED

Reason: ARM A reproduced the old C0 Present-positive baseline. The first ARM B attempt is preserved as `INVALID_PRE_WINE`, then one explicitly authorized corrected ARM B execution used the same staged B root and fixed only the environment import expression. Corrected ARM B reached `CreateSwapChainForHwnd rc=0`, `GetBuffer>0`, and `Present/Present1>0` with zero opcode/fault/reject counters. The native ntdll pair is not causal for the GetBuffer/Present disappearance.

## Contract Scope

- No source edits, rebuilds, profilers, input tests, production install, commit, or GOLDEN classification were performed.
- OLD immutable root: `reports/phase4-hollow-knight/focus-milestones-aa-runtime-admission-preflight-20260719-1559-NOT_GOLDEN/immutable/runtime-root/dist-arm64ec-spike/lib/wine`
- CURRENT tested ntdll root: `reports/phase4-hollow-knight/unity-jit-corridor-runtime-contract-correction-20260721-NOT_GOLDEN/runtime/dist-arm64ec-spike-unity-movnt-managed/lib/wine`
- Exact C0 DXMT overlay: `reports/phase4-hollow-knight/laneA-pixel-C0-try1-060521/dxmt-builtin-overlay`

## ARM A - OLD Baseline

Run: `reports/phase4-hollow-knight/laneA-native-ntdll-bisect-A-20260721-try1-232700`

Identity:

- `PRE-RUN-IDENTITY.json` SHA-256: `f40493cc08046e5ce6e76dc2bcfdb23c479f11e520ed3aee9802f3c548c8b29f`
- OLD dist clone: 4706 typed entries, 4702 files, 4 symlinks, 2,209,841,967 bytes, inventory SHA-256 `68b5f6c5f4985edb7aed2fbdf4554f8bc003d8c1285a3097b7e4e7497735ccf8`
- OLD warmed cache clone: 1 file, 76,940,200 bytes, inventory SHA-256 `9252db4ac0b9c04b25a7c9daf715346f98e162bb54dbb58125ca5f6bb8be8928`

Selected hashes after run:

- `aarch64-unix/ntdll.so`: `e1037bfae9e41108946aeb96c970c943853c46bb55ba25ce179564033f138123`
- `aarch64-windows/ntdll.dll`: `74cc1932316f6c808f43431e3bb23bf37faa51e1bce7d8d9025d2a62ecc24653`
- `x86_64-windows/ntdll.dll`: `b08516ba28c2d0d884b22673301bfc6eb3b5b48ee7458cd1248303289e431b78`
- `aarch64-unix/winemac.so`: `65bf6bf9b9d3e1030e1331a3b59b5c56282885fa7ac5469a617b26ff8546d0bc`

Runtime result:

- `mr-run` rc: `124`
- Elapsed: `1239s`
- `CreateSwapChainForHwnd rc=0`: observed
- `GetBuffer`: observed
- `Present/Present1`: observed; post-run parse counted `Present=500`, `Present1=500`
- `UNSUPPORTED`: `0`
- `MEMORY_FAULT`: `0`
- `reject`: `0`
- Focus/input/activation records: `0`
- First-Present pixel gate: `BLACK`, `non_black_px=0`, no verified snapshot created
- Prefix after scoped cleanup: absent

ARM A satisfies the old-good baseline gate for running ARM B.

## Shared DXMT Overlay

- `aarch64-unix/winemetal.so`: `fbfff7ce162c5db11d34b14181c31becc2feb219861faf013cd816a861db3004`
- `aarch64-windows/d3d11.dll`: `52f10210d45283a8b9b5e13699f72636de3bb79a8d19abb58af4c1a08c3b90cd`
- `x86_64-windows/d3d11.dll`: `f88868fcc464397b61549add45545ac48878e5b55b4c433cbeca9c9cb22689fd`
- Full overlay inventory SHA-256: `d7e36206e8bd82c21cbb9b1fe8f1c4ed6952d719c9d67336ea3f7590a890c88c`

## ARM B - Staged Single Variable

Staged root: `reports/phase4-hollow-knight/native-ntdll-bisect-20260721-NOT_GOLDEN/arm-B`

Identity:

- `PRE-RUN-IDENTITY.json` SHA-256: `eb7b2e629e3c7c610c8aa61c998f92d0ad53695c20186047b265ccc92aeb1480`
- B current warmed cache clone: 1 file, 22,606,624 bytes, inventory SHA-256 `26aced9e8d430ee129839cae182bac3cbb0279aac70953025b08558de590869e`
- A/B `lib/wine` entry count: A `3129`, B `3129`
- A/B different entries: exactly `2`

The two staged differences:

- `aarch64-unix/ntdll.so`: A `e1037bfae9e41108946aeb96c970c943853c46bb55ba25ce179564033f138123`, B `9a3f20b426adeeb86d70c06d0b7cfc9e687838e6eace5e43c282a19eb0363fc7`
- `aarch64-windows/ntdll.dll`: A `74cc1932316f6c808f43431e3bb23bf37faa51e1bce7d8d9025d2a62ecc24653`, B `3863660ac0b5f57c106d610d6a88d897a89a6c0ab626f890e66b4d572f02de4d`

Unchanged B controls:

- `x86_64-windows/ntdll.dll`: `b08516ba28c2d0d884b22673301bfc6eb3b5b48ee7458cd1248303289e431b78`
- `aarch64-unix/winemac.so`: `65bf6bf9b9d3e1030e1331a3b59b5c56282885fa7ac5469a617b26ff8546d0bc`
- DXMT overlay: exact C0 overlay listed above

## ARM B - Invalidated Launch Attempt

Run: `reports/phase4-hollow-knight/laneA-native-ntdll-bisect-B-20260722-try1-051058`

Evidence:

- `run.log` SHA-256: `465c74da918b508a0fee471951d549e8d5fb2b2773a5b837f32827a4a5326158`
- `run-contract.json` SHA-256: `5429adf0df32006d60721f1296b2313ce042924239cad97b53dbf4f26cb8d6b7`
- `final-child.json`: absent
- `mr-run.rc`: absent because the outer wrapper exited before recording it
- B prefix after failed preflight: absent
- Direct HK/Wine process scan after failure: `0`

Run-contract status:

- `status`: `BLOCKED`
- `blocker_count`: `4`
- `application.save_snapshot_manifest_sha256`: `canonical_save_path_authority_absent`
- `runner.branch_map.actxprxy`: `branch_input_absent`
- `runner.branch_map.crt_case_fusion`: `branch_input_absent`
- `runner.branch_map.wwise_observer`: `branch_input_absent`

The B ledger blocked before Wine. Therefore there is no B `CreateSwapChainForHwnd`, `GetBuffer`, `Present`, opcode, fault, or pixel evidence.

## ARM B - Corrected Authorized Execution

Run: `reports/phase4-hollow-knight/laneA-native-ntdll-bisect-B-corrected-20260722-try1-052828`

Authorization: user explicitly authorized one corrected B execution after preserving the invalid pre-Wine B evidence.

Prelaunch checks:

- Staged B root reused unchanged: `reports/phase4-hollow-knight/native-ntdll-bisect-20260721-NOT_GOLDEN/arm-B`
- `PRE-RUN-IDENTITY.json`: `eb7b2e629e3c7c610c8aa61c998f92d0ad53695c20186047b265ccc92aeb1480`
- `aarch64-unix/ntdll.so`: `9a3f20b426adeeb86d70c06d0b7cfc9e687838e6eace5e43c282a19eb0363fc7`
- `aarch64-windows/ntdll.dll`: `3863660ac0b5f57c106d610d6a88d897a89a6c0ab626f890e66b4d572f02de4d`
- `x86_64-windows/ntdll.dll`: `b08516ba28c2d0d884b22673301bfc6eb3b5b48ee7458cd1248303289e431b78`
- `aarch64-unix/winemac.so`: `65bf6bf9b9d3e1030e1331a3b59b5c56282885fa7ac5469a617b26ff8546d0bc`
- B cache clone inventory: `26aced9e8d430ee129839cae182bac3cbb0279aac70953025b08558de590869e`
- ARM A imported child environment names: `113`; corrected B final-child environment names: `113`; missing `0`, extra `0`
- Parent-only run-contract inputs present before launch: save/config/data path plus `actxprxy`, `crt_case_fusion`, and `wwise_observer`
- C0 control and DXMT swapchain trace enabled; Mono profiler variables absent

DXMT overlay note:

- ARM A identity records C0 overlay inventory as `d7e36206e8bd82c21cbb9b1fe8f1c4ed6952d719c9d67336ea3f7590a890c88c`.
- Live pinned overlay components still match the C0 hashes listed above, with 11 files and 51,339,056 bytes. The v1 tree-manifest aggregate for that same tree is `3c6d19868eadbedb3a0e5f0022fbe651462c86d97791acccd330e53ed91518c1`, which is a different serializer than the compact ARM identity value.

Run result:

- `mr-run` rc: `124`
- Elapsed: `1265s`
- `run.log` SHA-256: `45e1e41dc0d7a4f2de1074557ceb25fbc68f29d09be540d83d7cbf73a9becc65`
- `run-contract.json` SHA-256: `99ead579e2ea0499c41a5be1e90e35cc6a0ad64bdc9ee7bb22f4a5cbc08e2602`
- `run-contract` status: `READY`
- `run-contract` blockers: `0`
- `final-child.json` SHA-256: `9ca40a2f18a0823b03836e2e7521250ce335992556202eb29f29871a0d5d2f44`
- `CreateSwapChainForHwnd rc=0`: observed
- `GetBuffer`: `1`
- `Present`: `745`
- `Present1`: `745`
- `UNSUPPORTED`: `0`
- `MEMORY_FAULT`: `0`
- `reject`: `0`
- Focus/input/activation records: `0`
- First-Present pixel gate: `BLACK`, `non_black_px=0`, `colorful_px=0`; no verified snapshot created
- Post-run selected B hashes: unchanged
- Prefix after scoped cleanup: absent
- Direct HK/Wine process scan after run: `0`

## Conclusion

The old-good ARM A baseline was reproduced, and the corrected authorized ARM B run also reached `GetBuffer` and `Present/Present1` with zero faults, zero rejects, and unchanged staged hashes.

Machine verdict: `NTDLL_PAIR_EXONERATED`

Next isolation target: `winemac.so` `65bf6bf9b9d3e1030e1331a3b59b5c56282885fa7ac5469a617b26ff8546d0bc` -> `02b4d94ecb5827ea331decccb846f1bf6ff97df632ea28f6b40951794f8c9604`.

No source changes, rebuild, retry loop, production install, commit, or GOLDEN artifact was created.
