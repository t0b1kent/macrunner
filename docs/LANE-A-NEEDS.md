# Lane A — outstanding needs / blockers

## 2026-06-19: DXMT airconv Windows-target LLVM toolchain is missing

**Why it matters for Gate A / HK:**
- The only DXMT artifact that boots Hollow Knight past Mono reload is the overlay
  `reports/phase4-hollow-knight/laneA-A-fix-live2-try1-131139/dxmt-builtin-overlay/`.
- That overlay has a 33 MB `d3d11.dll` which statically links `airconv` (and therefore
  LLVM) so that `SM50Compile*` / `SM50Destroy*` / `SM50Get*` symbols are local to
  `d3d11.dll`.
- Fresh `scripts/build-dxmt.sh` produces a 5 MB `d3d11.dll` that imports `SM50*` from
  `winemetal.dll` (airconv lives in the Unix side / winemetal.so). HK then enters a
  repeating HyperBridge callback-exception loop during `Begin MonoManager ReloadAssembly`
  and never reaches `D3D11CreateDevice`.

**Conclusion:** the callback-exception loop is a **latent engine bug** triggered by
**cross-module airconv / SM50 calls** (`d3d11.dll` → `winemetal.dll` → unwind through
HyperBridge x64→ARM64EC callback boundary). The immediate workaround is to restore
the overlay's static-link layout: build airconv/LLVM **into** `d3d11.dll`. The engine
unwind bug should be fixed later in the engine terminal.

### Missing piece

`engine/dxmt/toolchains/llvm` must contain **Windows-target (x86_64-w64-mingw32)
static LLVM 15.0.7 libraries and headers** built from the upstream LLVM project.

Upstream DXMT CI recipe (`3Shain/dxmt/.github/workflows/ci.yml`):
- Clone `https://github.com/llvm/llvm-project.git --branch llvmorg-15.0.7` into
  `engine/dxmt/toolchains/llvm-project`.
- Build with:
  ```sh
  export LLVM_MINGW_DIR=llvm-mingw-20260505-ucrt-macos-universal
  export PATH="engine/toolchain/$LLVM_MINGW_DIR/bin:$PATH"
  cmake -B engine/dxmt/toolchains/llvm-build \
        -S engine/dxmt/toolchains/llvm-project/llvm \
        -DCMAKE_SYSTEM_NAME=Windows \
        -DCMAKE_INSTALL_PREFIX="$(pwd)/engine/dxmt/toolchains/llvm" \
        -DLLVM_HOST_TRIPLE=x86_64-w64-mingw32 \
        -DLLVM_ENABLE_ASSERTIONS=On \
        -DLLVM_ENABLE_ZSTD=Off \
        -DCMAKE_BUILD_TYPE=Release \
        -DLLVM_TARGETS_TO_BUILD="" \
        -DLLVM_BUILD_TOOLS=Off \
        -DCMAKE_SYSROOT="$(pwd)/engine/toolchain/$LLVM_MINGW_DIR" \
        -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
        -G Ninja
  cmake --build engine/dxmt/toolchains/llvm-build
  cmake --install engine/dxmt/toolchains/llvm-build
  ```

### Build script changes needed (ready in `scripts/build-dxmt.sh`)

Once `engine/dxmt/toolchains/llvm` exists:
- Pass `-Dbuild_airconv_for_windows=true` for the **x86_64-windows** cross build
  (this is the architecture HK actually loads in system32).
- Keep `-Dnative_llvm_path=/opt/homebrew/opt/llvm@15` (Darwin ARM64 host LLVM for
  the `airconv` CLI tool and native airconv lib used by Metal shader pipeline).
- Keep the mixed Wine builtin/native layout from patch
  `0002-macr-d3d11-no-builtin-postproc` (`d3d11` normal PE, `dxgi`/`winemetal`/
  `d3d10core` Wine builtin).

### Why x86_64 target specifically

Hollow Knight.exe is x86_64. MacRunner deploys `d3d11.dll` from
`engine/graphics/dist/dxmt/x86_64-windows/` into `C:\windows\system32`. The
`aarch64-windows` twins are only needed so Wine's builtin-loader can resolve the
builtin marker for ARM64X/EC handoff; they do not need airconv statically linked.

### Next step after toolchain exists

1. Rebuild DXMT x86_64 with `-Dbuild_airconv_for_windows=true`.
2. Verify `d3d11.dll` size jumps to ~30 MB and `SM50*` symbols are local (not
   imported from `winemetal.dll`).
3. Re-run HK: should pass Mono reload (~146 s), reach `D3D11CreateDevice`, and
   exercise the `af237cc` `ID3D11Fence` degrade fix (`CreateFence` → `S_OK`).
4. Snapshot the working pair (`dxmt` + `ntdll.so`) and commit.

### Latent engine bug (do not forget)

Even after the toolchain is restored, the cross-module airconv unwind loop must be
fixed in the engine terminal. Track separately as: **cross-module airconv call →
`RtlUnwind` callback-exception loop under HyperBridge ARM64EC callback boundary**.

## 2026-06-19 (later): static-airconv hypothesis DISPROVED; overlay is debug build

**Evidence**
- Overlay `reports/phase4-hollow-knight/laneA-A-fix-live2-try1-131139/dxmt-builtin-overlay/x86_64-windows/d3d11.dll`:
  - 32 MB, **no** `Wine builtin DLL` marker.
  - PE imports still show `SM50Compile*` / `SM50Destroy*` / `SM50Get*` coming from `winemetal.dll` (not local to `d3d11.dll`).
  - `strings` contains no LLVM library symbols (`LLVMPasses`, `LLVMTarget`, etc.).
  - Size is almost entirely `.debug_*` sections (`.debug_info` ~6 MB, `.debug_line` ~2.6 MB) and a larger `.text`/`.pdata` consistent with a debug/info-rich build, **not** a statically-linked airconv.
- Current 5 MB release build (`af237cc`) already passes `Begin MonoManager ReloadAssembly` and reaches `Loaded All Assemblies` (~152 s), prints Direct3D device info, and reaches PhysX backend selection.

**Conclusion**
The "Mono reload loop" was transient; the current blocker is **post-physics hang**, not cross-module airconv. Building a Windows-target static LLVM toolchain would not reproduce the overlay and is therefore **not a blocker** right now. Do not start the LLVM build until new evidence shows airconv static link is actually required.

**Next work**
1. Determine why the current build hangs after PhysX selection — present/fence path, input init, or missing temp/cursor directory (`Failed to save a temporary cursor file to 'C:\windows\temp\' because this directory does not exist`).
2. Verify whether `ID3D11Fence::Create` returns `S_OK` with `af237cc` (DXMT logging is currently no-op under Wine; may need targeted instrumentation or re-enable a minimal fence log).
3. Re-run Gate A with profiling once the post-physics hang is resolved and answer the original reset-pool questions (a) munmap share and (b) reset share.

## 2026-06-19 (post-temp-fix): current blocker is JIT unsupported-opcode loop, not fence/airconv

**Run**: reports/phase4-hollow-knight/laneA-A-fix-live2-tempfix-try1-205838 (180 s, current 5 MB d3d11.dll, C:\windows\temp created before run).

**Observations**
- Mono reload completes in ~121 s.
- Direct3D 11.0 [level 11.1] prints, so D3D11CreateDevice succeeds (DXMT logging is no-op under Wine).
- Loaded All Assemblies + PhysX backend selection complete.
- Process then spins until timeout.
- Live sample shows the x64 worker thread stuck in HyperBridge JIT execution: hb_jit_runtime_run -> run_jit_block_with_signal_guard -> hb_jit_helper_exec_two_block_loop -> exec_instr -> hb_memory_write / mach_vm_write.
- Two recurring UNSUPPORTED_OPCODE PCs:
  - 0x87efcb43f60: bytes 0f c2 c1 01 0f 50 c0 0f c2 fe 01 0f 50 cf a8 (SSE cmpps/cmpss family).
  - 0x87efdedc9ae: bytes 41 d1 d8 0f 43 c8 73 09 41 8b c9 81 e1 00 00 (rcr/cmovns family).

**Conclusion**
The post-physics hang is a HyperBridge x86_64 JIT opcode gap, not the ID3D11Fence::Create path. Until these opcodes are implemented or the guest path avoids them, HK cannot reach a sustained present frame, so Gate A profiling cannot answer the original munmap/reset percentages on a running frame.
