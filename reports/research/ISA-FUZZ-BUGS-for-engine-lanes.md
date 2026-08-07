# ISA semantic/fuzz BUGS — work-list for the engine lanes (from Gemini's correctness pass, 2026-06-07)

Gemini's ISA correctness program found REAL semantic bugs that the prior decode-only matrices missed.
These are captured here so they're not lost; the FIX is engine work (not Gemini's — it owns audio now).
Full evidence: `reports/research/HB-X86-32-ISA-COVERAGE-matrix.md`,
`reports/research/HB-X86-64-ISA-COVERAGE-matrix.md`, `reports/research/HB-X86-FUZZ-findings.json`.

## x86-32 → PE32 (xtajit) to fix
- **INC/DEC clobber CF** — `inc eax` (40), `dec esi` (65 4e): HyperBridge clears CF; INC/DEC MUST
  PRESERVE CF (only AF/OF/SF/ZF/PF change). High-value flag-semantics bug.
- **`push sp` under 66 prefix** (66 54): ESP delta wrong (expected …0ffe, actual …0ffc) — 16-bit
  operand-size stack adjust.
- **`and al, imm8` under 67 prefix** (67 24 57): interp_error 0/-5 — lift/interp fails with the
  address-size prefix.
- **`cwd` (99)** not implemented in the interpreter (interp_error 0/-5).
- Aggregate (x86-32 matrix): 291 missing, 242 mismatch, 1082 semantic_mismatch — prioritize the
  atlas families + the above confirmed fuzz hits.

## x86-64 → Lane A (xtajit64) to fix
- 931 mismatch + 1119 semantic_mismatch over 8,644 instrs (vs Unicorn).
- **RIP-relative addressing** base representation mismatch (P11).
- **Prefix-alias vector instructions** (P9).

## Routing note
These are NOT on the current critical path (graphics→window). Record only — PE32/Lane A pick them up
when they reach ISA-correctness work. Coordinator: re-surface to PE32 (Phase 3) and Lane A when the
window milestone clears. Tool to regenerate/extend: `tools/hb_isa_coverage` + `tools/hb_fuzz`.
