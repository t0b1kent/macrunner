# ABZU Fix A CS-forward verification — immutable pre-run manifest

- Classification: `NOT_GOLDEN`, single diagnostic verification; timeout `300s`, never `900s`.
- Attempt budget: exactly one; retry prohibited after `ATTEMPT-CONSUMED`.
- Root cause under test: HB and PE ntdll previously executed distinct critical-section bodies/wait protocols on the same object.
- Independent code change: with `MACRUNNER_HB_CS_FORWARD_NTDLL=1`, HB returns `FALSE` from its kernel32/kernelbase semantic only for `EnterCriticalSection`, `LeaveCriticalSection`, and `TryEnterCriticalSection`. Generic native PE dispatch then invokes the spec-resolved `NTDLL.Rtl*CriticalSection` target. Initialize/Delete/SpinCount and all other semantics are unchanged.
- Family guard: `tools/test_hb_cs_forward_source.py` covers all three siblings and both kernel32/kernelbase spec forwarders; PASS, SHA-256 `67fb683fd589b8b48fb1e8eb5889a17b275576befd349383f72b10b69e348b5f`.
- Main HEAD: `0b09f80511f51e0eb8f5d58da642ae0e6a866745`; KEEP reference `e7cca2e3`, no reset/revert/stash/commit.
- Source `unix/macrunner_hb.c` SHA-256: `4e16f99429b4d8548cd808a9bce93357fb8346f0281e6f12d0628926fab9a108`.
- Build command: `. ./config/env.sh && make -C engine/wine/build-arm64ec-spike dlls/ntdll/ntdll.so`.
- Build result: rc=0; only `unix/macrunner_hb.o` compiled and `ntdll.so` relinked; 13 pre-existing warnings, zero fix-line diagnostics, no `install skipped`.
- Built ntdll.so SHA-256: `3d7d65c1e2e14ef8413bbe021361ea65bfdf0f4c94a3f4159d6918b888db2b57`.
- Build log SHA-256: `973cc89d0188c2865180b275cba2dc525efb3bf28ba79b02ce9918578d81b0a9`.
- Execution root: `/Users/timurtoby/Documents/MacRunner/Main/MacRunner-abzu`; HEAD `f58219bb31dd808c4da6784f8cb4cd1775a88edf`; golden source is untouched.
- Runtime overlay: `artifacts/_abzu-guest-pc-movement-overlay-runtime-20260713/dist`.
- Pre-fix overlay ntdll.so SHA-256: `14d3563d105defde6efae10563e58b3ac7e88278b2533b96e882c8d6efacf2b5`; backed up and restored on launcher exit.
- Runner SHA-256: `d2d23492abd564693cab90cb0c49204b6f357f1af3f4244bd6cda6200eec8ee2`.
- Game SHA-256: `b9bdbc743742f45845b152df7663b042e2b5da5d318894584d70f6d63d6839e7`.
- DXMT d3d11/dxgi/winemetal SHA-256: `842fa9e7` / `30eb89bf` / `e49765a9`.
- Child environment is captured byte-exact in `CHILD-ENV.bin`; requested Fix A, JIT, syscall/callback, DXMT, WINEMSYNC, cache/memory flags are explicit. UI input tracing and old wait/heap observers are unset.
- Serialization uses Python regex parsing and fails closed before deployment/attempt consumption for Wine, wineserver/wineboot, ABZU/Hollow Knight, mr-run, make/ninja/cmake-build.
- Prefix policy: fresh disposable prefix, wineboot skipped, services and actxprxy enabled. Translation cache is preserved.
- Passive observations: at most one four-second host sample at game ages 120s and 270s. No global kill.
- Required verdict gates: ABBA; real CreateDXGIFactory1/EnumAdapters1/CopyAllDevices/D3D11CreateDevice HRESULTs; CreateSwapChainForHwnd/GetBuffer/RTV/Present1; zero terminal c0000005/c000007b/pc=0x60.

