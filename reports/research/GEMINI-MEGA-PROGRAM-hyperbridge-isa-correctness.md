# GEMINI MEGA-PROGRAM — HyperBridge translator correctness (x86-32 + x86-64), offline differential testing

**Agent:** Gemini (agy) — proven systems coder (native unarc macOS / `___chkstk_darwin` fix) + the
x86-32 ISA-coverage seed (`tools/hb_isa_coverage`, 11,077-instruction corpus, Unicorn+capstone diff).
**Horizon:** MONTHS. This is a PROGRAM, not a task. Phased, autonomous, evidence-driven.
**Date:** 2026-06-07
**Mission in one line:** become the **correctness oracle for the whole HyperBridge translator** — find
EVERY decode / lift / semantic divergence across the entire x86 ISA (32-bit AND 64-bit), with minimal
reproducers and a priority-ranked, regenerable gap matrix that PE32 (i386) and Lane A (x64) implement
against. Drive each ISA family to 100% decode + semantic parity vs the references.

## ⚡ AUTONOMY — FIND, CHAIN, DON'T STOP (read first)
- This is NOT "build one tool → report → wait". Chain Phase 0 → 6 without pausing. When a phase's
  matrix is green, immediately deepen the next family / the next ISA / the next oracle. The answer to
  "may I continue?" is always YES.
- You FIND and REPRODUCE (and build all the testing/CI infra to do so at scale). You do NOT edit the
  engine — the implementing lanes (PE32 = xtajit, Lane A = xtajit64/hb) apply fixes from your matrix.
  If you can express a fix as a precise spec/patch-proposal in the matrix, do so — but never edit their
  files.
- Only stop for the 3 STOP conditions at the end. "Done" is ASYMPTOTIC (the ISA is huge, semantics are
  deep) — keep closing gaps for months.

## 🟣 SURVIVAL DISCIPLINE (so you run for months without dying on context)
- ALWAYS use `ctx_execute` (language=javascript) for reading logs/corpora/engine source — NEVER raw
  cat/grep/tail on big files. `ctx_batch_execute` shell mode is broken here (spawn /bin/zsh ENOENT) —
  do not use it. Raw bytes stay out of your conversation; only derived answers enter.
- Checkpoint EVERY step to `reports/research/LANE-E-PROGRESS.md` (one line: `TIME · phase · action ·
  result(numbers) · next`). This is your resume anchor — a fresh thread continues from it.
- The living matrices are the durable output; the conversation is disposable.

## SCOPE / OWNERSHIP (hard — other lanes are live)
- **YOURS (edit):** `tools/hb_isa_coverage/**`, and you may create `tools/hb_fuzz/**` for the fuzzing
  layer. Living reports: `reports/research/HB-X86-32-ISA-COVERAGE-matrix.md`,
  `reports/research/HB-X86-64-ISA-COVERAGE-matrix.md`.
- **READ-ONLY references (never edit):** `engine/hyperbridge/**`, `engine/wine/dlls/xtajit/**` (PE32),
  `engine/wine/dlls/xtajit64/**` + `macrunner_hb.c`/`signal_arm64.c` (Lane A), `engine/wine/libs/capstone`
  (decode reference), `tools/hb_oracle/**` (Unicorn adapter — USE it; if it needs changes, extend
  inside your own dir or record the need, don't edit it in place while Lane A/PE32 may touch it).
- **NEVER touch** `engine/dxmt|graphics|vkd3d` (Lane D), `tools/triage` (Lane X).
- Commit ONLY your owned files (`feat(isa-coverage): …` / `feat(hb-fuzz): …`), never `git add -A`.

## REFERENCES (the 3 oracles)
1. **capstone** (`engine/wine/libs/capstone`) — decode/length/mnemonic/operand ground truth.
2. **Unicorn** (via `tools/hb_oracle` / the trace runner) — semantic execution ground truth
   (registers, EFLAGS, memory, FPU/SSE state) for a real x86 CPU model.
3. **The x64 interpreter golden oracle** — MacRunner's own reference for shared-IR semantics.
A HyperBridge result is correct only when it matches the relevant oracle(s); flag every divergence.

## PHASES (work top-to-bottom; always have a next item; loop until asymptotic)
### Phase 0 — Framework hardening (generalize the seed)
Turn `tools/hb_isa_coverage` into a robust, re-runnable differential-testing FRAMEWORK: clean CLI,
deterministic corpus generation, capstone + Unicorn + oracle backends behind one interface, minimal-
reproducer extraction (shrink failing instructions to the smallest divergent bytes), machine-readable
summary lines for CI. No more one-off scripts.

### Phase 1 — x86-32 DECODE coverage → 100% vs capstone
Exhaustively enumerate the i386 opcode map: 1/2/3-byte, all legacy/REP/LOCK/segment/operand/address-
size prefixes, full ModR/M + SIB + displacement space, x87 FPU, MMX, SSE/SSE2 (the report already shows
676/706 SSE decoded — close the remaining 30 and the rest). Every MISSING/MISMATCH → matrix row with
bytes + expected(capstone) vs actual(HyperBridge). Gate: 0 unexplained decode divergences.

### Phase 2 — x86-32 SEMANTIC coverage → parity vs Unicorn + oracle
For every decodable i386 instruction, diff register/EFLAGS/memory effects vs Unicorn (and the oracle
where applicable). Nail the hard, high-value classes the seed already flagged: MOVZX/MOVSX transitions,
MUL/IMUL/DIV/IDIV, ADC/SBB (incl. LOCK), ROL/ROR/RCL/RCR + shift flag edge cases (CF/OF exactness),
string ops (MOVS/STOS/SCAS/CMPS, DF direction, REP bounds), BCD, x87 control/status word, SSE rounding/
denormals/MXCSR. Minimal repro per divergence. Gate: semantic-diff green for each implemented family.

### Phase 3 — x86-64 DECODE + SEMANTIC coverage (feeds Lane A)
Repeat Phases 1–2 for the 64-bit ISA: REX/REX.W, RIP-relative addressing, 64-bit operand sizes, the
x64 SSE/SSE2 surface, and the x64 EFLAGS/RFLAGS exactness. Publish `HB-X86-64-ISA-COVERAGE-matrix.md`.
This directly hands Lane A a precise x64 work-list.

### Phase 4 — Corpus-driven weighting (what real software actually runs)
Pull real instruction streams from the target-ladder binaries (and the i386 installers/games:
Notepad++, KeePass, Diablo, Terraria, etc.) — parse PE `.text`, weight the matrices by real-world
frequency, and emit a per-title ISA fingerprint (which families each title exercises). So PE32/Lane A
implement what unblocks real titles first.

### Phase 5 — Continuous fuzzing (find the deep ones)
Build `tools/hb_fuzz`: structured + randomized instruction generation with shrinking, differential
against all oracles, dedup + triage of divergences. Run long campaigns; surface the non-obvious tail
(prefix combinations, boundary displacements, undefined-flag behaviors).

### Phase 6 — CI gate + regression DB
Wire the decode-diff + semantic-diff + fuzz as a permanent CI gate (like the x64 gate). A regression DB
of known-divergences so coverage can NEVER silently regress; matrices auto-update on each run.

## LIVING OUTPUT
`reports/research/HB-X86-32-ISA-COVERAGE-matrix.md` + `HB-X86-64-ISA-COVERAGE-matrix.md`: every opcode
family → decode status / semantic status / priority / minimal repro / which lane owns the fix. These ARE
the work-lists for PE32 and Lane A. Keep them current as you go.

## STOP conditions (ONLY these — otherwise loop the phases for MONTHS)
1. Both ISA matrices show 0 unexplained decode divergences AND semantic parity for all implemented
   families, wired into CI (asymptotically "done").
2. A hard blocker needing another lane's files (engine edits) — record the exact need in the matrix +
   `reports/research/PE32-NEEDS.md` (i386) or a Lane A note (x64) and KEEP WORKING other phases.
3. Operator decision on a real trade-off (e.g. "match an undefined-flag quirk exactly or not?").

In your FINAL message each run: which families moved to green (with numbers), what new divergences you
found (with minimal repros), and `CONTINUE <next phase/family>` (almost always) / `BLOCKED <why>` /
`DONE` (only when STOP #1 holds).
