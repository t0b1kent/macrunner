# Gate A — Fence-S_OK Milestone Snapshot

Date: 2026-06-20 06:49
MacRunner parent commit: `e25b7a7`

## Working pair

| Component | Path | Commit/SHA |
|-----------|------|------------|
| ntdll.so (A) | `engine/wine/dist-arm64ec-spike/lib/wine/aarch64-unix/ntdll.so` | `871ae0278f3fb743` |
| DXMT d3d11.dll | `engine/graphics/dist/dxmt/aarch64-windows/d3d11.dll` | `bdeef11705d120df` |
| DXMT submodule | `engine/dxmt` | `3e4a38b` |

## What works (verified in `A-visible-fence-4`)

- HK boots past Mono reload to `Input initialized`.
- `ID3D11Device::CreateFence` called with `Flags=0x0`, `feature_flags=0x820`.
- `dxmt::CreateFence` returns `S_OK` (`hr=0x0`, `local_kmt=0x0`) via degraded local MTLSharedEvent-backed fence path.
- `winemac.drv` aarch64-windows PE + aarch64-unix `.so` now rebuild and deploy from source.

## Known next blockers (DO NOT address until coordinator hands off)

1. `get_win_data(Unity HWND)` returns `NULL` in `winemetal.so`, so `macdrv_view_create_metal_view` falls back to orphan NSWindow. Coordinator is running a burst to determine the exact cause and design; no blind `get_win_data` changes until then.
2. No `Present`/swapchain activity observed within 600 s visible run after `Input initialized`; this should be re-evaluated after (1) is fixed.

## Rebuild notes

- `clang: posix_spawn failed via winegcc` was a transient `dlltool` resolution problem. Fix: ensure `/tmp/macrunner-build-bin` (or equivalent) contains per-target `*-dlltool` symlinks pointing at `llvm-dlltool`, and the llvm-mingw toolchain is first in `PATH`.
