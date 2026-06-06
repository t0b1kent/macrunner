# HyperBridge Offline i386 ISA Coverage Tool

This directory contains the tools for checking x86-32 ISA coverage and semantic execution transitions of MacRunner's HyperBridge interpreter vs Capstone and Unicorn.

## Architecture

1. **`probe.c`**: Links with `engine/hyperbridge/libhyperbridge.a` and decodes/lifts instructions from hex string inputs, printing comprehensive decoded instruction details as a flat JSON line.
2. **`Makefile`**: Standard compilation recipe to build the `hb_x86_probe` binary.
3. **`coverage.py`**: A Python runner that:
   - Compiles the custom probe binary.
   - Generates a systematic opcode corpus across the entire x86-32 space (one-byte, two-byte, three-byte, x87 FPU, SSE, and legacy instruction forms).
   - Extracts real-world unique instructions from 32-bit executables (e.g. KeePass and Notepad++ installers).
   - Compares Mnemonics, Instruction Lengths, and Operand Structures between Capstone and MacRunner.
   - Executes semantic state comparisons against a Unicorn emulator oracle using the statically compiled trace runner.
   - Generates a prioritized gap matrix Markdown report at `reports/research/HB-X86-32-ISA-COVERAGE-matrix.md`.

## Usage

Ensure you have Capstone and Unicorn python packages installed, and source the project env:

```bash
. ./config/env.sh
python3 tools/hb_isa_coverage/coverage.py
```

### Options

- `--pe-limit <N>`: Maximum unique instructions to extract from each test executable (default: `5000`).
- `--sem-limit <N>`: Maximum decoded cases to validate semantically against Unicorn (default: `2000`).

## CI Integration

The script prints a machine-readable summary line to `stdout` upon completion:

`SUMMARY: passed=X missing=Y mismatch=Z semantic_mismatch=W`

Exit code is `0` if it runs successfully. This summary line and exit code can be integrated as a CI gate.
