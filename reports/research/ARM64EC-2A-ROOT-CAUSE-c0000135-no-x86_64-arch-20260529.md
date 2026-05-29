# ARM64EC 2a — ROOT CAUSE of c0000135: spike build has NO x86_64 arch; test PE is plain x64

**Date:** 2026-05-29
**Inputs:** capture `reports/research/ARM64EC-2A-ntdll-c0000135-context-20260529.md`
(Cline capture-only, clean) + on-disk arch check. Analysis = operator-side via context-mode.
**VERDICT:** The `c0000135` is **NOT** a bug in `xtajit64/cpu.c`, NOT a missing file, NOT a
prefix/registration problem. It is a **build-arch mismatch**: the spike Wine was built
`--enable-archs=arm64ec,aarch64,i386` (**no `x86_64`**), but the test binary `hello_x64.exe` is a
**plain x86_64 PE**, which takes the WoW64 x64-guest path and needs an `x86_64-windows` ntdll that
this build never produced. Lane = **Codex/Opus (build + loader)**. NOT Cline, NOT a cpu.c edit.

## Hard evidence
1. **Arch dirs actually built** (on disk):
   `aarch64-unix/`, `aarch64-windows/`, `i386-windows/` — **no `x86_64-windows/`, no `x86_64-unix/`.**
   `scripts/build-wine-arm64ec-spike.sh:47` → `--enable-archs=arm64ec,aarch64,i386`.
2. **`x86_64-windows/ntdll.dll` → does not exist.** `x86_64-unix/` → does not exist.
3. **Test PE is plain x64:** `tests/native-fixtures/build/hello_x64.exe` (13824 B) — a normal
   x86_64 PE, not an arm64ec hybrid PE.
4. **Trace sequence (from the capture, lines ~3454–3507):** main wine proc `002c` loads the EC
   system DLLs fine — `wow64.dll` (3455), `wow64win.dll` (3457), `xtajit64.dll` (3462),
   `ntdll.dll` (3465), `kernelbase`, `ucrtbase`, `win32u`, … all OK. Then a **new child process
   `0024`** resolves `hello_x64.exe` (3506) and the **bootstrap of that child** dies:
   ```
   3507: wine: failed to load .../aarch64-unix/ntdll.dll error c0000135
   ```
   The fatal line has no thread prefix → it is the freshly-spawned child's bootstrap loader, not
   the main process. `xtajit64.dll` mapped fine earlier; our cpu.c never ran (no `0x6502`).
5. **Secondary noise (note, not primary):** repeated
   `err:virtual:try_map_free_area mmap() error Cannot allocate memory, range 0x7ffffff...` during
   high-VA reservation. May be benign retries OR may bite again once x86_64 is added — watch it.

## Why this happens (mechanism)
A plain x86_64 PE on an ARM64 host runs as a **WoW64 x64 guest** emulated by `xtajit64.dll`. That
path needs the **x86_64 guest ntdll** (`x86_64-windows/ntdll.dll` + its unix side). The arm64ec
build packs x64 *EC* code as arm64x **inside** `aarch64-windows/ntdll.dll` — that serves the
**ARM64EC hybrid** model, NOT the classic WoW64-x64 guest. So when the child tries to stand up the
x64-guest environment for the plain-x64 exe, the required x86_64 ntdll view is absent → the child
bootstrap fails → `c0000135`. (Part 1 GREEN still holds: it proved arm64ec *links* + exports — a
different layer. No contradiction.)

## Two ways forward — Codex/Opus decision (do NOT pick blindly)
- **(A) If the target is running ordinary x86_64 game PEs (most real games are plain x64):**
  add `x86_64` to the archs → `--enable-archs=arm64ec,aarch64,x86_64,i386`, rebuild the spike
  tree. Then `x86_64-windows/ntdll.dll` exists, the WoW64+xtajit64 path can bring up the x64
  guest, and the Part 2a BeginSimulation reach-proof can finally be observed. **Most likely the
  one we want.**
- **(B) If the target is specifically the ARM64EC in-process hybrid model:** the test binary must
  be an **arm64ec hybrid PE**, not a plain x64 exe. Build/obtain an arm64ec fixture and retest.
- Either way, then re-run the same capture; expect to reach `xtajit64` ProcessInit/BeginSimulation.
- Also confirm the `try_map_free_area` mmap errors don't block the x64 4GB reservation after (A).

## Lane / discipline
- This is **build + loader**, Codex/Opus lane. Cline did the capture cleanly (heredoc fix worked)
  and correctly did NOT analyze. The verdict here is evidence-based (arch dirs + trace), not status.
- `xtajit64/cpu.c` Part 2a lifecycle work remains valid but is **gated** until the loader brings up
  the x64 guest far enough to call `BeginSimulation`.
