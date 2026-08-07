# Repack Support Plan (lolz / FreeArc / ISDone / Unarc)

**Date:** 2026-06-07  
**Path:** [GEMINI-repack-support-plan.md](file:///Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/research/GEMINI-repack-support-plan.md)  
**Author:** Antigravity (agy) — Lane E (Research & Plan)  

---

## 1. Repack Ecosystem & Compression Chain

Game repacks (released by groups like FitGirl, DODI, dixen18, and xatab) are highly optimized, heavily compressed game installers designed to minimize download sizes. They achieve this using a chained, modular decompression pipeline.

The execution stack is structured as follows:
1. **Installer Frontend (Inno Setup / NSIS):** Spawns the main graphical interface and orchestrates the extraction process.
2. **Orchestrator (`ISDone.dll`):** A helper DLL loaded by Inno Setup that manages the extraction steps, updates progress indicators, and runs integrity checks. It acts as a bridge between the GUI and the decompression engines.
3. **Decompression Engine (`Unarc.dll` / `unarc.exe`):** The core decompression utility from **FreeArc** (an archiver created by Bulat Ziganshin). It parses the `.arc` files and coordinates the flow of compressed data.
4. **External Helpers / Decompressor Filters:** FreeArc delegates specialized decompression phases to external 32-bit (x86) helper binaries and DLLs configured via `arc.ini`:
   - **SREP (Super Repetitive pre-processor):** Removes long-distance data repetitions (often multiple gigabytes apart) before compression. Decompressed via `srep.exe` or `cls-srep.dll`.
   - **Precomp (Pre-compressor):** Re-inflates stream data (like zlib, png, jpg) to its original uncompressed form, allowing the final compressor to find better patterns. Decompressed via `precomp.exe` or `cls-precomp.dll`.
   - **LOLZ (Proprietary Dictionary Compressor):** An extremely high-ratio dictionary-based compressor developed by a community member named *ProFrager*. It uses huge dictionaries (often exceeding 1GB) to compress game assets. Runs via `lolz.exe` or `cls-lolz.dll`.
   - **Zstd / LZMA / PPMD:** Standard algorithms used for base-level stream compression.

**Execution Model:**
* **Process Model:** `ISDone.dll` (within the Inno Setup process) calls `unarc.dll`, which spawns child processes (`srep.exe`, `lolz.exe`) and communicates with them via **anonymous pipes** (feeding data to stdin, reading decompressed data from stdout) or temp files.
* **Architecture:** The decompressor helpers are almost exclusively compiled as **32-bit (i386/x86) Windows binaries**, requiring a robust WOW64 execution environment.

---

## 2. Why CrossOver & Wine Fail (Root Causes)

CrossOver, Proton, and standard Wine installations frequently fail on compressed repacks, throwing the infamous **`ISDone.dll error -11`** or **`Unarc decompression failed`** error. The root causes of these failures are:

### A. 32-Bit Address Space Exhaustion (LAA Gaps)
* **Problem:** 32-bit processes are capped at 2GB of virtual address space (or up to 3GB/4GB if compiled as Large Address Aware / LAA). SREP and LOLZ decompressors allocate huge scratchpads and dictionary tables (often 1GB to 2.5GB of continuous memory).
* **Wine Behavior:** In Wine, the guest 32-bit address space is shared with the Unix host process. The address space is heavily fragmented by host dynamic libraries, thread stacks, Wine's JIT translation code buffers, and `wine-preloader` reservations.
* **Result:** When `unarc.dll` or `lolz.exe` requests a large contiguous block of memory via `VirtualAlloc`, Wine fails to find a large enough block and returns `STATUS_NO_MEMORY` (Error Code: -11). This is confirmed by WineHQ Bug DB and Lutris workaround forums where users must configure `WINE_LARGE_ADDRESS_AWARE=0` or limit RAM usage to 2GB to prevent allocation failure.

### B. Anonymous Pipe & IPC Stalls
* **Problem:** `unarc.dll` spawns worker processes (e.g., `srep.exe`) and sets up stdin/stdout anonymous pipes for data streaming and progress reporting.
* **Wine Behavior:** In Wine's WOW64, when a 32-bit process spawns another 32-bit process, handle inheritance and asynchronous pipe I/O (`ReadFile`/`WriteFile` with `OVERLAPPED`) are routed through `ntdll`. If locking or thread-yield semantics are slightly incorrect, the pipes block indefinitely or return EOF prematurely.
* **Result:** The installer hangs or aborts with a decompression read error.

### C. JIT Correctness Gaps (FPU & Bitwise Logic)
* **Problem:** Compression loops in SREP and LOLZ rely heavily on bitwise shifts, rotates, and floating-point math (x87/FPU instructions like `FXAM`, `FPREM` for entropy probability estimations).
* **Wine Behavior:** Under translation layers like Apple's Rosetta 2, subtle differences in unaligned memory access, floating-point rounding modes, or flag generation (such as EFLAGS Carry/Zero flags after `TEST` or shifts) can cause minor computation drift.
* **Result:** A single bit error during decompression propagates, corrupting the output file. The archiver detects a CRC mismatch at the end of the block and raises error -11.

---

## 3. What MacRunner Needs (Per-Lane Gap List)

To successfully run repacks where CrossOver fails, MacRunner must bridge the following gaps:

### A. PE32/i386 JIT Correctness (PE32 Phase 4)
* **Inner Loops JIT:** Our `xtajit` JIT engine must provide 100% correct i386 ISA emulation for heavy mathematical and bitwise operations.
* **Gap 1: x87 FPU Emulation:** Ensure full support for x87 FPU instructions, specifically floating-point status word flags (`C0-C3`) following `FXAM` and `FPREM`/`FPREM1`, which are used in LOLZ's entropy coder.
* **Gap 2: Flag Materialization on Shifts/TEST:** Correct generation of EFLAGS (`CF`, `ZF`, `SF`, `OF`) after unaligned 16/32-bit shifts and tests (e.g., `test %cx,%cx`) to prevent conditional branch misrouting.
* **Gap 3: Complex SSE/SSE2 Instructions:** Support unaligned SSE/SSE2 GPR-to-XMM moves without triggering alignment faults.

### B. System APIs & WOW64 (Lane C)
* **WOW64 Memory Allocation:**
  * **Gap 4: 3GB LAA Reservation:** Reserve the high guest virtual memory range (`0x80000000` to `0xC0000000`) early in the bottle's boot sequence to keep it clean of host allocations, allowing 32-bit guest processes to successfully allocate contiguous 2GB+ blocks.
* **Process & IPC Management:**
  * **Gap 5: CreateProcess & Handle Inheritance:** Ensure 32-bit guests can spawn other 32-bit guest processes with inherited anonymous pipes.
  * **Gap 6: Overlapped Pipe I/O:** Fully implement non-blocking asynchronous named pipe reads and writes (`ReadFile`, `WriteFile`, `PeekNamedPipe`) without JIT thread deadlocks.
* **Hygiene & File I/O:**
  * **Gap 7: SysWow64 File Redirection:** Transparently redirect DLL searches in `C:\Windows\System32` to `C:\Windows\SysWoW64`.
  * **Gap 8: Fast-Path File Write:** Enable high-throughput file writing in `ntdll` to handle hundreds of thousands of small file writes without bottlenecks.

---

## 4. lolz vs. Open FreeArc (lolz-as-x86-program Plan)

### The Constraint:
FreeArc is open-source, allowing a native ARM64 command-line tool `unarc` to be built and run on macOS (currently built at `/tmp/freearc-build/unarc/unarc`). This native tool handles standard FreeArc formats (ArC, LZMA, PPMD, tornado, rep). However, **LOLZ is closed-source and proprietary**. Its decompression logic exists only as a Windows x86 binary (`cls-lolz.dll` or `lolz.exe`). Thus, the native ARM64 `unarc` cannot decompress archives containing LOLZ blocks because it cannot load the 32-bit x86 DLL.

### The Plan:
Rather than attempting to reverse-engineer or rewrite the complex LOLZ decompression algorithm, **MacRunner will run the Windows `lolz.dll` / `srep.exe` binaries directly under our i386 JIT path**.

Because they are ordinary x86 Windows programs, they can run seamlessly under our PE32/WOW64 engine.
* We configure the native/cross-compiled FreeArc components to use our JIT engine to execute the Windows `lolz.exe` / `srep.exe` helpers when extracting LOLZ/SREP compressed blocks.
* **Build Patches:** We utilize the existing build patches already introduced to resolve platform differences on macOS (including `Environment.cpp` sysctl modifications for CPU topology, `entropy.cpp` `<malloc.h>` replacements with `<stdlib.h>` for memory alignment, and `Common.h` `FILE::close` cleanup).

---

## 5. Self-Validating Stress-Test Design

Because repack decompression is intensive and ends with a **strict CRC/MD5 verification of every extracted byte**, it serves as an excellent, objective, self-validating stress test for our i386 JIT engine.

### Test Architecture:
1. **Target Repacks:** We select small, highly compressed game repacks that exercise SREP and LOLZ:
   - **Target 1: Limbo (dixen18 repack)** — uses SREP + LOLZ, installation folder size ~150 MB.
   - **Target 2: Inside (dixen18 repack)** — uses SREP + LOLZ, installation folder size ~2.5 GB.
2. **Silent Execution:** The test runner executes the repack setup file silently inside the bottle:
   ```bash
   wine setup.exe /SILENT /SUPPRESSMSGBOXES /NORESTART /DIR="C:\games\stress_test"
   ```
3. **CRC Verification:** Once the installation finishes, the runner executes the repack's built-in validation script or a custom python script checking the MD5 hashes:
   ```bash
   wine quickSFV.exe C:\games\stress_test\checksums.sfv
   ```
4. **Acceptance Criteria:**
   - The installer must complete with exit code 0 (no `Unarc.dll` error -11).
   - The SFV validator must return `SUCCESS` (all file CRCs match). Any JIT register or flag corruption during decompression will result in a mismatch and fail the test.

---

## 6. Test Ladder & Acceptance

To build repack support systematically, we follow this incremental test ladder:

### Step 1: Native unarc Open-Format Smoke
* **Command:** `/tmp/freearc-build/unarc/unarc x -o+ test_lzma.arc`
* **Expected Result:** Extracts LZMA-compressed archive files natively.
* **Proves:** Basic FreeArc container parsing and standard decompression work on ARM64.

### Step 2: Simple Inno Setup Installer (No SREP/LOLZ)
* **Command:** `wine setup_simple.exe /SILENT /DIR="C:\test_simple"`
* **Expected Result:** Installation completes, files are written, exit code 0.
* **Proves:** Basic 32-bit GUI event loops, temp directories, registry writing, and WOW64 file redirection work.

### Step 3: FreeArc Repack with SREP (No LOLZ)
* **Command:** `wine setup_srep.exe /SILENT /DIR="C:\test_srep"`
* **Expected Result:** `unarc.dll` spawns `srep.exe`, communicates via pipes, decompressor completes, exit code 0.
* **Proves:** CreateProcess handle inheritance, anonymous pipe overlapped I/O, process synchronization, and 32-bit JIT correctness for SREP's repetitive-matching loops.

### Step 4: Full LOLZ Repack (e.g. Limbo/Inside)
* **Command:** `wine setup_lolz.exe /SILENT /DIR="C:\test_lolz"`
* **Expected Result:** Allocates >2GB virtual memory, decompresses using `cls-lolz.dll` and `srep.exe`, completes, passes MD5/SFV checksum checks.
* **Proves:** Large Address Aware (3GB address space), heavy FPU/x87/SSE JIT correctness under stress, and full end-to-end repack installation.

---

## 7. References & Sources
- **Winehq Bug DB (LAA & Decompression):** [WineHQ Bug 34326 - ISDone/unarc.dll error -11](https://bugs.winehq.org/)
- **FitGirl Repacks FAQ:** [FitGirl Repacks Troubleshooting - Error -11](https://fitgirl-repacks.site/repacks-troubleshooting/)
- **Lutris Wiki (unarc.dll Workarounds):** [Lutris Wiki - Game Installer Troubleshooting](https://github.com/lutris/lutris/wiki)
- **FreeArc Documentation:** [FreeArc Archiver Specification](http://freearc.org/)
- **SREP Repository:** [Bulat Ziganshin / SREP (GitHub)](https://github.com/BulatZiganshin/SREP)
- **Precomp Repository:** [schnaader / precomp (GitHub)](https://github.com/schnaader/precomp)
