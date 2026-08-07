# HB Graphics Bring-up Diagnosis — 2026-06-01

## Run

- Artifact: `reports/phase4-hollow-knight/run-20260601-034642-graphics-diagnosis-default90/`
- Command shape: `scripts/mr-run.sh engine/wine/dist-arm64ec-spike Hollow Knight.exe 90 -- -logFile -`
- Env: `MACRUNNER_HB_X64_LOADER=1`, `MACRUNNER_HB_BACKEND=jit`, `MACRUNNER_HB_JIT_DIRECT_MEM=1`, `WINEDEBUG=+loaddll,+module,+d3d11,+dxgi`
- Result: timeout `143`, cleanup/prune `0`, no JIT fallback/fault/runtime failure markers.

## Findings

Unity reaches `UnityPlayer.dll` and loads `d3d11.dll` / `dxgi.dll`, but only as import dependencies during `UnityPlayer.dll` attach. No renderer/device creation is observed in this 90s run:

- `GfxDevice=0`
- `Direct3D=0`
- `CreateSwapChain=0`
- `winemetal=0`
- `DXMT=0`
- `macrunner-hb-jit-fallback=0`
- `JIT codegen failed=0`
- `MEMORY_FAULT=0`
- `UNSUPPORTED_OPCODE=0`
- `runtime-fail=0`

The `D3D11CreateDevice=3` / `CreateDXGIFactory=6` counts are import-thunk setup, not runtime calls:

```text
29142: d3d11.dll!D3D11CreateDevice redirected 0000087EF6870050 -> 0000087EF6800C14
29143: registered import thunk d3d11.dll!D3D11CreateDevice ... guest=0x6f0000001b80
29144: rewrote import d3d11.dll!D3D11CreateDevice target=0000087EF6800C14 guest=00006F0000001B80
29147: dxgi.dll!CreateDXGIFactory2 redirected 0000087EF6560030 -> 0000087EF653031C
29150: dxgi.dll!CreateDXGIFactory redirected 0000087EF6560010 -> 0000087EF6530164
```

The loaded graphics DLLs are the stock Wine/wined3d frontends from `engine/wine/dist-arm64ec-spike`, not DXMT:

```text
29136: build_module loaded L"\\??\\C:\\windows\\system32\\d3d11.dll" ... 0000087EF67F0000
29137: Loaded L"C:\\windows\\system32\\d3d11.dll" ...: builtin
31493: process_attach (L"d3d11.dll",...) - START
31494: process_attach (L"dxgi.dll",...) - START
31495: process_attach (L"wined3d.dll",...) - START
```

Binary marker check confirms the active dist DLLs are wined3d-backed, while DXMT lives separately:

```text
engine/wine/dist-arm64ec-spike/lib/wine/x86_64-windows/d3d11.dll: Failed to create wined3d...
engine/wine/dist-arm64ec-spike/lib/wine/x86_64-windows/dxgi.dll: Failed to create a wined3d device...
engine/graphics/dist/dxmt/x86_64-windows/d3d11.dll: dxmt / MTLD3D11 markers
engine/graphics/dist/dxmt/x86_64-windows/dxgi.dll: dxmt / MTLDXGI markers
engine/graphics/dist/dxmt/x86_64-windows/winemetal.dll: winemetal / CreateMetalViewFromHWND markers
```

After module attach, the run continues into Mono startup:

```text
54056: Mono path[0] = '.../Hollow Knight_Data/Managed'
54086: Mono config path = '.../MonoBleedingEdge/etc'
```

## Conclusion

The immediate graphics-path blocker is not a failed DXMT device call. The default Hollow Knight run has not entered actual renderer creation yet, and the current throwaway `mr-run` environment would route any future D3D11/DXGI calls to stock Wine/wined3d instead of DXMT/winemetal.

Next work item: stage the DXMT `d3d11.dll`, `dxgi.dll`, `d3d10core.dll`, and `winemetal.dll` frontends into the run path, force native DLL load order for the throwaway prefix, then rerun Hollow Knight and the DX11 fixture to validate `device -> swapchain -> winemetal -> first frame`.

## Update — DXMT staged, renderer still not reached

- Artifact: `reports/phase4-hollow-knight/run-20260601-064714-hk-dxmt-long120/`
- Result: timeout `143`, cleanup/prune `0`, no JIT fallback/codegen/helper-fault markers, no access violation, no bad-image, no datatype-misalignment.
- DXMT path: `macrunner-hb-dxmt-unixlib-bridge=1`, DXGI attach success, d3d11 attach success, winemetal attach success.
- Renderer markers: `GfxDevice=0`, `D3D11CreateDevice=0`, `CreateDXGIFactory=0`, `CreateSwapChain=0`.
- DXGI sidecar: `Hollow Knight_dxgi.log` only reports Metal cache path fallback.

Wait tracing in `run-20260601-065130-hk-dxmt-waittrace60/` shows no failed waits or obvious deadlock. The infinite startup waits are paired with semaphore releases and the process remains alive until timeout.

Artificial block-limit evidence in `run-20260601-065324-hk-dxmt-blocklimit60/` stops before renderer creation inside UnityPlayer:

```text
MacRunner HyperBridge x64 dll block limit at pc=0x87efed9439d
macrunner-hb-run-exit: label=dll status=00000102 reason=block-limit pc=0x87efed9439d blocks=61a9 steps=1b437
```

Decoded location:

```text
UnityPlayer.dll RVA 0x19d439d / VA 0x1819d439d
mov rax, qword ptr [rbx]
test rax, rax
je 0x1819d43ad
movabs r10, <xfg hash>
call qword ptr [rip + 0x2dd83] ; helper table at 0x181a02130 -> 0x1819e89f0
add rbx, 0x8
cmp rbx, rdi
jne 0x1819d4395
```

Current diagnosis: DXMT is loaded and ready, but Unity has not reached graphics device creation. The evidence now points to UnityPlayer/Mono throughput before `GfxDevice`, specifically hot CFG/XFG indirect-call dispatch work, rather than a failed DXMT device/swapchain call.

## Update — PE ntdll route active, main thread fails before block 0

- Artifact: `reports/phase4-hollow-knight/run-20260601-094359-hk-dxmt-pejit-amd64detect120/`
- Result: timeout path moved to a hard x64 thread-entry failure: `hb_BaseThreadInitThunk_unix_return status=c0000005 ret=0 blocks=0 steps=0`.
- Route evidence: `main-amd64-detected=3`, `main-entry-override=3`, `base-thread-thunk-install=3`, `hb_BaseThreadInitThunk_enter=3`.
- Attach evidence: DXMT-side dependencies attach under HyperBridge before the main thread failure (`winemetal.dll`, `d3d11.dll`, UnityPlayer `PROCESS_ATTACH`).
- Renderer markers remain absent: `GfxDevice=0`, `D3D11CreateDevice=0`, `CreateSwapChain=0`, `Present=0`.

Current diagnosis: graphics still has not started, but the blocker is now earlier and sharper than the previous throughput loop. The PE-routed x64 main thread enters `unix_macrunner_hb_x64_thread_entry`, logs `hb_x64_thread_entry_begin entry=0x140001264`, then returns `c0000005` before any translated block or `macrunner-hb-run-exit` marker. Next diagnostic target is `macrunner_hb_run_x64` thread-entry stack/ABI/signal setup.

## Update — fault classes cleared, clean pre-graphics timeout

- Artifact: `reports/phase4-hollow-knight/run-20260601-1041-hk-dxmt-transient-ir-lifetime180/`
- Result: timeout `143`, cleanup/prune `0`, no `c0000005`, no `c000007b`, no `MEMORY_FAULT`, no JIT codegen/helper faults, no JIT fallback, no unsupported opcodes.
- Fixes validated in this batch: safe helper-backed JIT memory by default, successful Mono `guest_module_remap`, and retained transient IR lifetime for persistent JIT blocks.
- Renderer markers remain absent: `GfxDevice=0`, `D3D11CreateDevice=0`, `CreateSwapChain=0`, `Present=0`.

Current diagnosis: Hollow Knight is no longer blocked by the thread-entry crash, dynamic Mono exec overlay, or dangling IR helper fault. The remaining graphics blocker is clean pre-renderer throughput/location. Next run should enable heartbeat/progress ranking on the clean safe-JIT path and identify the hot PC family before Unity reaches renderer creation.

## Update — hot pre-GfxDevice JIT promotions active

- Validation: `reports/phase4-hollow-knight/test-20260601-1240-hb-self-loop-fusion.log` = `391 passed, 0 failed`.
- Staged build: `reports/phase4-hollow-knight/build-20260601-1241-ntdll-self-loop-fusion.log` = build/copy `0`.
- Latest run: `reports/phase4-hollow-knight/run-20260601-1242-hk-dxmt-self-loop-fusion60/`.
- Result: timeout `143`, cleanup/prune `0`, no `c0000005`, no `c000007b`, no `MEMORY_FAULT`,
  no JIT codegen/helper faults, no JIT fallback, no unsupported opcodes.
- Renderer markers remain absent: `GfxDevice=0`, `D3D11CreateDevice=0`, `CreateSwapChain=0`, `Present=0`.
- New safe-default JIT promotions firing on the pre-renderer path:
  `bounded-byte-scan=456`, `copy-scan-counted=167`, `byte-compare-loop=19`, `self-loop=76`.
- The former rank-one Unity stride-store loop at UnityPlayer RVA `0x2cb480`
  (`mov dword [r8], -1; add r8, 0x18; cmp r8, rax; jne`) is now covered by `self-loop`
  promotion and dropped out of the top hot list.

Current diagnosis: graphics still has not started, but the active blocker keeps moving forward as
finite hot CPU loops are promoted. The next hot cluster is helper-heavy compare work in `ucrtbase`
RVA `0x329910/0x329916` and Mono/Unity initialization blocks (`mono-2.0-bdwgc.dll` around RVA
`0x4ee3e0`, UnityPlayer `0x6c44xx` / `0x2838xx`). Continue ranking/promoting the finite hot IR
families until Unity reaches `GfxDevice` / DX11 device creation.

## Update — self-loop16 cleared Mono zero-fill, still pre-GfxDevice

- Validation: `reports/phase4-hollow-knight/test-20260601-1301-hb-self-loop16.log` = `391 passed, 0 failed`.
- Staged build: `reports/phase4-hollow-knight/build-20260601-1302-ntdll-self-loop16.log` = build/copy `0`.
- Latest short ranking run: `reports/phase4-hollow-knight/run-20260601-1303-hk-dxmt-self-loop16-60/`.
- Latest long check: `reports/phase4-hollow-knight/run-20260601-1310-hk-dxmt-long-lowtrace180/`.
- Result: clean timeout, no access violation, bad image, memory fault, JIT codegen/helper fault,
  JIT fallback, or unsupported opcode.
- Renderer markers remain absent after 180s:
  `GfxDevice=0`, `D3D11CreateDevice=0`, `CreateSwapChain=0`, `Present=0`.
- Safe-default JIT promotions now firing on the pre-renderer path:
  `bounded-byte-scan=456`, `copy-scan-counted=167`, `byte-compare-loop=19`, `self-loop=96`.
- The Mono RVA `0x4ee3e0` 11-op XMM zero-fill loop is now covered by the widened self-loop helper and
  dropped out of the hot list.

Current diagnosis: DXMT is staged and loaded, but Unity remains CPU-bound before renderer startup.
The next finite hot sets are `ucrtbase.dll` RVA `0x329910/0x329916`, UnityPlayer `0x6c44xx`,
`0x2838xx`, and the XFG dispatch loop around UnityPlayer RVA `0x19d439d`.

## Update — low-trace proof, wait callers, and Unity comparator promotion

- Validation: `reports/phase4-hollow-knight/test-20260601-1555-hb-i32-comparator-equal.log`
  = `395 passed, 0 failed`.
- Staged build: `reports/phase4-hollow-knight/build-20260601-1556-ntdll-i32-comparator-equal.log`
  = build/copy `0`.
- Low-trace proof: `reports/phase4-hollow-knight/run-20260601-1418-hk-dxmt-lowtrace300/`
  timed out cleanly after 300s with DXMT loaded and no graphics markers.
- Wait caller probe: `reports/phase4-hollow-knight/run-20260601-1504-hk-dxmt-waitcaller75/`
  maps parked worker waits to UnityPlayer RVA `0x577c92/0x577f44` and the zero-timeout poll to
  UnityPlayer RVA `0xcba8b2`.
- New exact helper-backed JIT work: direct stack now works with `MACRUNNER_HB_JIT_DIRECT_STACK=1`
  while `MACRUNNER_HB_JIT_DIRECT_MEM=0`; added Mono null-qword scan fusion and Unity RVA `0x649910`
  int32 comparator entry/equal-path fusion.

Current diagnosis: graphics still has not started because Unity remains before renderer creation.
The waits seen so far are Unity worker parking/polling, not failed DXMT calls. Continue with evidence
on the remaining Unity scheduler/sort hot loops (`0x649910`, `0x6c44xx`, `0x2838xx`, `0x19d4xxx`) and
only switch to DXMT device/swapchain code after a real `GfxDevice` / D3D11 create marker appears.

## Update — comparator fusion now fires, still not enough

- Validation: `reports/phase4-hollow-knight/test-20260601-1615-hb-i32-comparator-nearcache.log`
  = `395 passed, 0 failed`.
- Staged build: `reports/phase4-hollow-knight/build-20260601-1616-ntdll-i32-comparator-nearcache.log`
  = build/copy `0`.
- Run: `reports/phase4-hollow-knight/run-20260601-1618-hk-dxmt-i32-comparator-nearcache90/`.
- Result: clean timeout, cleanup/prune `0`, no access violation, bad image, memory fault,
  JIT codegen/helper fault, JIT fallback, or unsupported opcode.
- Renderer markers remain absent:
  `GfxDevice=0`, `D3D11CreateDevice=0`, `CreateSwapChain=0`, `Present=0`.
- Fusion evidence: `i32-less-tiebreaker=1` now fires alongside the earlier scan/self-loop fusions.

Current diagnosis: the comparator fusion is correct and active, but Unity is still pre-renderer.
Continue with the next finite hot families: XFG indirect-call loop around `0x19d439d` and sort
partition loops around `0x2838xx`.

## Update — comparator exact helper safe, plateau unchanged

- Validation: `reports/phase4-hollow-knight/test-20260601-1648-hb-i32-comparator-exact.log`
  = `395 passed, 0 failed`.
- Staged build: `reports/phase4-hollow-knight/build-20260601-1649-ntdll-i32-comparator-exact.log`
  = build/copy `0`.
- Run: `reports/phase4-hollow-knight/run-20260601-1651-hk-dxmt-i32-comparator-exact90/`.
- Result: clean timeout, cleanup/prune `0`, no access violation, bad image, memory fault,
  JIT codegen/helper fault, JIT fallback, or unsupported opcode.
- Renderer markers remain absent:
  `GfxDevice=0`, `D3D11CreateDevice=0`, `CreateSwapChain=0`, `Present=0`.
- Fusion evidence remains active: `i32-less-tiebreaker=1`, plus existing scan/self-loop fusions.

Current diagnosis: the `0x649910` comparator is no longer a generic IR-dispatch helper, but the same
pre-renderer plateau remains. The next useful target is not more comparator work; inspect the XFG
indirect-call loop around `0x19d439d` or the sort partition/callback loops around `0x2838xx`.

## Update — runner preloader fixed, XFG branch target narrowed, still pre-GfxDevice

- Runner diagnosis: post-build `mr-run` batches initially produced only prefix-sync output. A direct
  process sample showed the Wine app child stuck at macOS `_dyld_start` inside the temporary preloader
  copy. Setting Wine's existing `WINELOADERNOEXEC=1` bypasses that path and restores Wine process
  creation plus HyperBridge bootstrap; `scripts/mr-run.sh` now defaults it for `dist-arm64ec-spike`.
- JIT change: ARM64 codegen now handles absolute/RIP-relative 64-bit `CALL/JMP [mem]` branch targets
  through the safe HyperBridge memory-read helper, then keeps stack/PC update native. Broad
  `MACRUNNER_HB_JIT_DIRECT_MEM` remains off.
- Validation: `reports/phase4-hollow-knight/test-20260601-1725-hb-abs-branch-target.log`
  = `395 passed, 0 failed`; staged `ntdll.so` build/copy
  `reports/phase4-hollow-knight/build-20260601-1727-ntdll-abs-branch-target.log` = `0`.
- Hollow Knight run: `reports/phase4-hollow-knight/run-20260601-1820-hk-dxmt-noexec-abs-branch-target240/`
  timed out cleanly with cleanup/prune `0`; DXMT bridge and `winemetal`/UnityPlayer attach are present,
  with zero JIT fallback, codegen/helper fault, memory fault, or unsupported opcode.
- Renderer markers remain absent:
  `GfxDevice=0`, `D3D11CreateDevice=0`, `CreateSwapChain=0`, `Present=0`.
- Targeted sort probe: `reports/phase4-hollow-knight/run-20260601-1828-hk-dxmt-ir-sort-283876180/`
  confirms Unity RVA `0x283876` is already covered by bounded-byte-scan fusion.

Current diagnosis: graphics still has not started because Unity remains in pre-renderer CPU init.
Next finite hot families are the Unity string binary-search loop (`0x6c4451/0x6c4483/0x6c449e`),
sort/callback loop heads (`0x283d31/0x283d52/0x283d91`), and residual XFG `0x19d439d`.
