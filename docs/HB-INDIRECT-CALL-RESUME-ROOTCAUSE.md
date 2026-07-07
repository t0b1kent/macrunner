# HB intercepted indirect-call resume root cause

Date: 2026-07-07

Scope: read-only diagnosis. No Wine render routing or DISPROVED paths were changed.

## Shared blocker

Two lanes hit the same HB class:

- ABZU: `MACRUNNER_HB_WINEMETAL_X64_DLLMAIN` lets `winemetal.dll` run DllMain, then `EnumAdapters1` reaches the winemetal unixlib path, but the first successful intercepted indirect call is followed by a `pc=0x60` re-dispatch loop.
- HK: DXMT vtable calls such as `call [rax]` / `call [r9+0x18]` reach the same null-ish indirect-call shape after swapchain `GetBuffer` / `Present`.

## Evidence cross-check

`/Users/timurtoby/Documents/MacRunner/Main/MacRunner-abzu/reports/dualdata/ABZU-GL-ROUTE-DIAG.md` sections 5-6 split the graphics issue into two problems:

- Problem A is GL noise / wined3d side-load.
- Problem B is the real D3D11 blocker: winemetal DllMain / unixlib handle plus the HB intercepted-indirect-call resume defect.

The report specifically says the `0x376d` return-site block is skipped after `call [mem]`, then a null `ret` to `pc=0x60` on the syscall stack is mis-recovered back into dispatch.

`reports/phase5-dxmt-x64-present/FINDINGS-x64-present-unixlib.md` independently confirms:

- `winemetal` unixlib handle wiring is fixed by running `winemetal.dll` DllMain.
- The first `WMTCopyAllDevices` dispatch succeeds.
- The loop persists with the correct real handle, so it is not a null-handle or out-param-copy issue.
- In `winemetal!unix_call_init`, `call [0x9008]` at rva `0x3767` should return to rva `0x376d`.
- `macrunner_hb_finish_import` was observed setting `pc=ret_addr=0x376d`, `rsp+=8`, and `rax=status`.
- Control still re-enters `unix_call_init` at rva `0x3700`, then later faults at `pc=0x60` inside syscall recovery.

## Code facts

Indirect-call codegen:

- `engine/hyperbridge/src/hb_arm64_codegen.c:1688` implements native x64 indirect call emission.
- `engine/hyperbridge/src/hb_arm64_codegen.c:1693-1698` computes the target, pushes `instr->guest_addr + instr->guest_len`, stores `ctx->pc=target`, then runs `emit_indirect_ic_probe()`.
- `engine/hyperbridge/src/hb_arm64_codegen.c:5353-5405` is the helper fallback for call operands.
- `engine/hyperbridge/src/hb_arm64_codegen.c:5461-5469` `hb_jit_call_target64()` pushes `ret_addr` and sets `ctx->pc/rip=target`.

Import / native-dispatch return:

- `engine/wine/dlls/ntdll/unix/macrunner_hb.c:3805-3811` `macrunner_hb_finish_import()` sets `rax=value`, `rsp+=8`, `ctx->pc=ret_addr`, and `rip=ret_addr`.
- That makes the import finish path look correct at the register level.

Runtime lookup:

- `engine/hyperbridge/src/hb_runtime.c:1483-1487` `find_block()` is exact: it returns a block only when `block->guest_addr == addr`.
- `engine/hyperbridge/src/hb_runtime.c:3176-3182`, `3199`, and `3545-3550` look up `ctx->pc` through `block_cache_find()` / `find_block()`.
- `engine/hyperbridge/src/hb_runtime.c:1590-1601` `update_indirect_ic()` records `target->guest_addr` and `target->native_code + 16`; it is a block-entry cache, not a mid-block resume mechanism.

Fault recovery:

- `engine/wine/dlls/ntdll/unix/signal_arm64.c:1868-1925` `handle_syscall_fault()` handles any fault with SP inside the syscall frame.
- If `jmp_buf` exists, it longjmps.
- If no `jmp_buf` exists, it unconditionally sets return state through `__wine_syscall_dispatcher_return` / `__wine_pe_x18_thunk` instead of rejecting invalid low PCs.
- The observed loop has `pc=0x60`, `lr=0x60`, no `jmp_buf`, and SP inside syscall, so this path converts a bad low-PC fault into another dispatcher return and repeats.

## Root cause

This is a two-part HB control-flow defect:

1. After an intercepted indirect call returns, HB does not guarantee exact execution at the guest return-site PC. The import/unixlib finish path writes the correct `ret_addr`, but the next runtime/JIT step can still re-enter or reuse a block-entry path instead of executing the return-site block/epilogue. In the winemetal trace, the correct return site is `unix_call_init+0x6d` (`0x376d`), but execution re-enters `unix_call_init+0x0` (`0x3700`).
2. The subsequent low/native fault (`pc=0x60`) is then mis-classified by `handle_syscall_fault()` as recoverable syscall-frame fallout. With no `jmp_buf`, it routes back to the syscall dispatcher return path, forming the re-dispatch loop.

The first part loses forward progress; the second part hides the loss by looping instead of surfacing a hard HB resume fault.

## Minimal patch candidate

Do not fix this in DXGI, winemetal, or ABZU-specific routing.

Patch candidate A: exact return-site resume in HB.

- Add an explicit `hb_resume_exact_pc` / `resume_after_intercepted_call` state, set by the import/native-dispatch finish path when it writes `ctx->pc=ret_addr`.
- On the next HB runtime step, require an exact block/cache entry for `ctx->pc`.
- If `ctx->pc` is not a block entry but is a valid decoded address inside the current function, split/build a block starting exactly at `ctx->pc`, or fall back to interpreter/decode from that address until the next block boundary.
- Do not reuse an indirect-IC cached `native_code + 16` entry unless the cached block's `guest_addr` equals the exact resume PC.
- Clear `ctx->indirect_ic_guest_addr/native_code` when leaving an intercepted native/import dispatch, unless the exact-resume lookup revalidates it.

Patch candidate B: syscall low-PC recovery fuse.

- In `handle_syscall_fault()`, before the no-`jmp_buf` dispatcher-return recovery, detect impossible low PCs for HB-controlled dispatch, e.g. `PC_sig(context) < 0x10000` or not a valid code address, with SP inside syscall and no `jmp_buf`.
- For that case, do not return through `__wine_syscall_dispatcher_return`.
- Return `FALSE` or raise a hard HB resume fault with a diagnostic marker such as `macrunner-hb-syscall-lowpc-resume-reject`.
- This is a safety fuse, not the primary fix. It prevents infinite loops and makes missed exact-resume failures visible.

## Acceptance gates for the real patch

- ABZU with `MACRUNNER_HB_WINEMETAL_X64_DLLMAIN=1`: no `pc=0x60` re-dispatch loop; winemetal DllMain/unixlib handle remains live; `EnumAdapters1` advances to real DXGI/D3D11 path.
- HK: vtable indirect calls after swapchain (`GetBuffer` / `Present`) do not hit the `pc=0x60` loop.
- No broad changes to GL routing or WineD3D side-load behavior in this patch.
- No regression to normal syscall fault recovery where a valid `jmp_buf` exists.
