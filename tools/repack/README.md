# Repack CRC Stress-Harness & Test Spec

This directory contains the repack smoke test harness for verifying archive decompression correctness and guest WOW64 subsystem stability in MacRunner.

---

## 1. Test Ladder & Status

| Rung | Target | Focus | Status | Key Engine Dependencies |
|---|---|---|---|---|
| **Rung A** | `test_open.arc` | Native decompression | 🟢 **PASS** | POSIX file I/O operations only. No guest JIT translation required. |
| **Rung B** | `setup.exe` (NSIS/Inno) | 32-bit installer stub | 🔴 **BLOCKED** | • **32-bit JIT translation:** basic i386 translation.<br>• **Process/IPC:** `CreateProcessW` entrypoint routing. |
| **Rung C** | FreeArc + SREP | Spawning helpers, pipes | 🔴 **BLOCKED** | • **Handle Inheritance:** spawning worker processes (`srep.exe`) with inherited stdout/stdin.<br>• **Async Pipes:** Overlapped named pipe reading/writing without JIT thread hangs. |
| **Rung D** | Full LOLZ repack | Heavy FPU & Large Memory | 🔴 **BLOCKED** | • **x87 FPU Emulation:** `FXAM`/`FPREM` status flags (`C0-C3`) for the LOLZ entropy coder.<br>• **TEST flags:** ZF/CF flags on unaligned shifts/tests.<br>• **3GB LAA Memory:** WOW64 large virtual memory allocations. |

---

## 2. Running the Smoke Tests

### Rung A: Native FreeArc Smoke (Runnable Today)
Extracts a pre-packaged open-format FreeArc archive (`test_open.arc`) using our natively compiled macOS ARM64 `unarc` binary, then validates the MD5 checksums of all extracted files against a reference manifest.

```bash
./tools/repack/run_repack_smoke.sh --native
```

### Rung B: Inno/NSIS Installer Smoke (Blocked)
Attempts to run a 32-bit setup wizard under Wine in silent mode to verify basic WOW64 API routing.

```bash
./tools/repack/run_repack_smoke.sh --wine
```

### Rung C: SREP Pipe Smoke (Blocked)
Spawns the `srep.exe` helper filter and passes data over anonymous pipes.

```bash
./tools/repack/run_repack_smoke.sh --rung-c
```

### Rung D: Full LOLZ Repack Stress Test (Blocked)
Stress tests the WOW64 guest JIT compiler's x87/FPU math and Large Address Aware (LAA) memory allocation limits by running a full LOLZ repack installer (e.g. Limbo/Inside).

```bash
./tools/repack/run_repack_smoke.sh --rung-d
```

---

## 3. Reference Documents
- **Test Specification:** [GEMINI-repack-testspec.md](file:///Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/research/GEMINI-repack-testspec.md)
- **Repack Support Plan:** [GEMINI-repack-support-plan.md](file:///Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/research/GEMINI-repack-support-plan.md)
- **Engine Backlog:** [ENGINE-REQUIREMENTS-BACKLOG.md](file:///Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/research/ENGINE-REQUIREMENTS-BACKLOG.md)
