# Lane A: Unity+0x2b5605 Race/Init-Order Diagnosis

Date: 2026-07-02

## Outcome Distribution

Warm uninstrumented default, N=8:
- 3/8 pre-swapchain timeout.
- 5/8 swapchain timeout.
- 0/8 `UnityPlayer.dll+0x2b5605`.
- 0/8 other `macrunner-hb-runtime-fail`.
- 0/8 GetBuffer/RTV/real Present.

Warm `MACRUNNER_HB_JIT_DIRECT_STORE_FENCE=1`, N=8:
- 8/8 swapchain timeout.
- 0/8 `UnityPlayer.dll+0x2b5605`.
- 0/8 other `macrunner-hb-runtime-fail`.
- 0/8 GetBuffer/RTV/real Present.

Caveat: the persistent AOT key does not encode codegen environment flags. Future fence/no-fence A/B must use separate `MACRUNNER_HB_TRANSLATION_CACHE_ROOT`s per codegen-env variant.

## RSI Producer Walk

Fault instruction:

```asm
UnityPlayer.dll+0x2b5605: mov rax, qword ptr [r8 + rsi*8 + 0x488]
```

Producer in the same function:

```asm
UnityPlayer.dll+0x2b5587: mov rsi, qword ptr [rdx]
```

The observed bad `rsi=0x107a0a4a0` came from descriptor field `[rdx]`; it is consumer-visible corrupted data, not a mapping/protect fault.

Direct callsite on the fault backtrace:

```asm
UnityPlayer.dll+0x6bbe8d: mov edx, dword ptr [rax + 0x180]
UnityPlayer.dll+0x6bbe93: dec edx
UnityPlayer.dll+0x6bbe95: mov dword ptr [rax + 0x180], edx
UnityPlayer.dll+0x6bbea2: movsxd rdx, dword ptr [rax + 0x180]
UnityPlayer.dll+0x6bbead: lea rdx, [rax + rdx*24]
UnityPlayer.dll+0x6bbeb1: call 0x1802b5570
```

Descriptor slot producer:

```asm
UnityPlayer.dll+0x6bbe28: mov rax, qword ptr [rdx + 0x628]
UnityPlayer.dll+0x6bbe2f: mov qword ptr [rdx + rcx*8], rax
UnityPlayer.dll+0x6bbe33: mov rax, qword ptr [rdx + 0x630]
UnityPlayer.dll+0x6bbe3a: mov qword ptr [rdx + rcx*8 + 8], rax
UnityPlayer.dll+0x6bbe3f: mov dword ptr [rdx + rcx*8 + 0x10], ebp
```

`c628/c630` are initialized to zero at `0x2b5472/0x2b5479` and updated by `0x2b5630/0x2b563b` from descriptor `[rdx]/[rdx+8]`. No local `_Init_thread_header/_Init_thread_footer` import or obvious MSVC magic-static guard appears around `0x2b5570`.

Targeted `MACRUNNER_HB_TRACE_UNITY_ORIGIN=1 MACRUNNER_HB_TRACE_UNITY_EVENT_OBJECT=1` run:
- Run: `reports/phase4-hollow-knight/laneA-laneA-unity-origin-20260702-171352-try1-171352`
- Normal lifecycle consumed descriptor slot `{0,0,1}` at `0x2b5570`.
- `0x2b5605` executed successfully; no runtime fault.
- Real `CreateSwapChainForHwnd rc=0`; no GetBuffer.

## HB Finding/Fix

The fence discriminator exposed a coverage gap: `MACRUNNER_HB_JIT_DIRECT_STORE_FENCE=1` fenced the generic/alignment-checked direct-store path, but not offset/fused stores. Critical Unity stores such as `[rsi+0x40]` and `[r8+0x628]` use offset forms.

Patch:
- `engine/hyperbridge/src/hb_arm64_codegen.c`
- Moved env-gated post-store `DMB ISH` into `emit_direct_mem_store_from_x20_base`.
- Added the same env-gated fence to `emit_direct_mem_store_zero_off`.
- Removed the duplicate fence from `emit_direct_mem_store_from_x20_tso`.

This is diagnostic/infrastructure correctness only; default behavior remains unchanged unless `MACRUNNER_HB_JIT_DIRECT_STORE_FENCE=1`.

## Verification

Build/deploy:
- HyperBridge rebuilt successfully.
- Forced Unix `ntdll.so` relink against new `libhyperbridge.a`.
- Deployed/codesigned `engine/wine/dist-arm64ec-spike/lib/wine/aarch64-unix/ntdll.so`.
- Deployed signed hash: `48566904f9b703831d90bd6d5903e79694a1156fdd3f52b8cba6449057fbaa4a`.

Test caveat:
- Broad `hb_test_runner` is not clean in this workspace: observed `446-449 passed`, `20-23 failed`, mostly code-size and host-permission/JIT cases. This patch is env-gated and the broad failures were not isolated to this change.

Live patched fence coverage:
- Populate run: `reports/phase4-hollow-knight/laneA-laneA-fencecov-populate-20260702-172446-try1-172501`
- Warm run: `reports/phase4-hollow-knight/laneA-laneA-fencecov-warm-20260702-173252-try1-173337`
- Filtered classifier: rung 11 `PRESENT_MISSING`, self-check PASS with real swapchain evidence.
- Raw markers: `CreateSwapChainForHwnd rc=0`, no GetBuffer/RTV, no real Present, no runtime-fail, no `+0x2b5605`.

## Verdict

`UnityPlayer.dll+0x2b5605` is a one-shot upstream descriptor corruption/race, not a mapping/protect fault. The exact consumer reads `rsi` from descriptor `[rdx]`; the normal trace shows the descriptor should be `{0,0,1}` in this path.

Store-drain coverage was incomplete and is now fixed for the diagnostic fence knob, but even full direct-store fence coverage does not advance past the post-swapchain wall. The current verified frontier remains rung 11: real factory, D3D11 device, `CreateSwapChainForHwnd rc=0`, then no GetBuffer/RTV.
