# Kimi MEGA Master Brief — Full Engine Audit (read-only)

**Scope**: Comprehensive read-only audit всего MacRunner engine pока Codex закрывает Phase H bug #7. Goal: find architectural issues, race conditions, silent failures, missed bug classes, test gaps. Output: documented findings prioritized by severity.

**Duration**: 1-2 weeks intensive read + analyze, 10-15 sessions estimated.

**Autonomy level**: Maximum в audit work. Полная свобода в interpretation, prioritization, recommendations.

**CRITICAL CONSTRAINT**: This is **READ-ONLY audit**. You may NOT modify engine source code. Engine is Codex's active territory. Document findings, do not patch. If you find a bug, write a report with reproduction; Codex decides fix.

**Conflict risk with Codex**: ZERO if you respect the read-only rule. Audit is purely additive (new docs).

**Strategic value**: Critical. Codex is in narrow Heisenbug loop on bug #7 indirect call / mprotect race. Fresh independent eyes на the entire engine may find:
1. The actual root cause of current Heisenbug что Codex пропускает
2. OTHER serious bugs we haven't hit yet (silent failures, edge cases)
3. Architectural validation что existing fixes (bugs #1-7) sound
4. Test coverage gaps that will bite KeePass / 1С / AutoCAD

This is **multiplier work**: 1-2 weeks audit может сэкономить **months** of reactive debug for next 5-10 apps.

---

## 0. Mandatory reading first

1. [/Users/timurtoby/Documents/MacRunner/Main/MacRunner/AGENTS.md](../AGENTS.md) — **🎯 Zeroth Principle**, family audit protocol, patch-by-evidence rule
2. [/Users/timurtoby/Documents/MacRunner/90-architectural-discoveries-fundamental-bugs.md](file:///Users/timurtoby/Documents/MacRunner/90-architectural-discoveries-fundamental-bugs.md) — **all 7 bugs found so far**, in detail
3. [/Users/timurtoby/Documents/MacRunner/91-notepad-plus-plus-journey.md](file:///Users/timurtoby/Documents/MacRunner/91-notepad-plus-plus-journey.md) — journey context
4. [/Users/timurtoby/Documents/MacRunner/85-calc-journey-17-layers.md](file:///Users/timurtoby/Documents/MacRunner/85-calc-journey-17-layers.md) — earlier Calc journey
5. Your prior briefs (audio, AOT, networking) — methodology pattern you're applying here

These give you **complete context** about engine evolution, known bug classes, methodology. Audit will be much sharper after reading.

---

## 1. Strategic context — why audit now

### Current state of Codex's Phase H journey

Codex has been closing bug #7 (cross-arch isolation) for the past several sessions. Architectural foundation is done:
- Primary signal handler installed early
- Host PROT_EXEC properly stripped from x64 guest pages
- EntryPoint correctly resolved (not image base)
- 850,000+ x64 blocks executed correctly через HyperBridge

But Codex hit a **Heisenbug**: with `MACRUNNER_HB_TRACE_HOST_EXEC=1` trace enabled, x64 .text properly strips host exec. With it disabled, mach_vm_region shows PROT_EXEC restored somehow. **Diagnostic trace changes observed behavior** — classic race / silent failure pattern.

Codex is in narrow tunnel: adding more probes, building, running, reading logs. Each iteration takes 5-10 minutes. He's been at this for hours.

**Fresh eyes можно** скорее всего find within hours what's happening:
- Is there an mprotect() return value being ignored somewhere?
- Is there a code path Codex hasn't traced that restores exec?
- Is the policy not applied to ALL views uniformly?
- Is there race between threads modifying same range?
- Is mach_vm_protect semantics different from mprotect in our case?

You read all relevant code in one pass with full context. Codex iterates narrow.

### Bigger value: find what's NOT broken yet

Bugs #1-7 were found through **app pressure**: an app hit them, we found and fixed. This is reactive. **Proactive audit** can find bug classes BEFORE they kill the next app:

- KeePass 1.43 (x86) will hit different paths
- 1С Тонкий клиент uses different APIs heavily
- AutoCAD/Revit/Photoshop exercise graphics stack
- Modern games hit anti-cheat, multiplayer

Every fundamental bug fixed proactively = days/weeks saved on next app.

---

## 2. Engine scope (89,468 lines, 165 files)

### Primary audit targets

| Subsystem | Files | Lines | Priority |
|---|---|---|---|
| **HyperBridge core** (engine/hyperbridge/) | ~40 .c/.h | ~25,000 | CRITICAL |
| **Wine ntdll Unix side** (dlls/ntdll/unix/) | ~30 .c/.h | ~30,000 | CRITICAL |
| **macrunner_hb.c** | 1 | 3,049 | CRITICAL |
| **virtual.c** | 1 | 7,721 | CRITICAL (current Heisenbug here) |
| **signal_arm64.c** | 1 | 2,841 | CRITICAL (cross-arch boundary) |
| **Wine PE ntdll** (dlls/ntdll/) | ~20 .c | ~15,000 | HIGH |
| **kernelbase** (dlls/kernelbase/) | ~30 .c | ~10,000 | HIGH (ACCESS_MASK family) |
| **win32u** (dlls/win32u/) | ~30 .c | ~10,000 | MEDIUM |
| HyperBridge tests | hb_test_runner.c | ~9,000 | MEDIUM (coverage gaps) |

### Specific hot files

These have been heavily modified during Phase H and deserve special attention:

1. **engine/wine/dlls/ntdll/unix/virtual.c** — memory mapping, mprotect, view registry, range tracking. Current Heisenbug is here. 7721 lines.
2. **engine/wine/dlls/ntdll/unix/signal_arm64.c** — primary signal handler, cross-arch trampoline, fault routing. 2841 lines.
3. **engine/wine/dlls/ntdll/unix/macrunner_hb.c** — HyperBridge integration, callback dispatch, IAT thunks. 3049 lines.
4. **engine/hyperbridge/src/hb_decode_x64.c** — x64 decoder (where bugs #1, #4, #5 lived)
5. **engine/hyperbridge/src/hb_arm64_codegen.c** — JIT (bug #6 was here)
6. **engine/hyperbridge/src/hb_interpreter.c** — interpreter (must produce identical results to JIT)
7. **engine/hyperbridge/src/hb_flags.c** — eFLAGS computation (lazy + raw)
8. **engine/hyperbridge/src/hb_memory.c** — guest memory access primitives

---

## 3. CRITICAL CONSTRAINT — read-only

**You MUST NOT modify any engine source code during audit.**

Allowed:
- Read all source files end-to-end
- Run tests (`./scripts/test-hyperbridge.sh`) to understand behavior
- Run probes (use existing trace knobs MACRUNNER_HB_TRACE_*) только для evidence
- Write new files in `docs/ENGINE-AUDIT-*.md`
- Write tests in NEW file `engine/hyperbridge/tests/audit/` (separate dir from Codex's territory) если нужны reproduction tests

NOT allowed:
- Edit any existing engine file (engine/hyperbridge/src/*, engine/wine/dlls/**/*)
- Modify existing tests
- Modify build scripts
- Commit code patches to engine

If you find a bug — write a **report file** with:
- Symptom
- Reproduction steps
- Probable root cause (your analysis)
- Suggested fix scope (not the fix itself)
- Confidence level (high/medium/low)

Codex reads your reports and patches. You retain audit role.

---

## 4. Audit dimensions (10 topics, each gets its own doc)

You audit engine across these dimensions. Each gets a dedicated finding doc.

### Dimension 1: Memory model coherence
File: `docs/ENGINE-AUDIT-01-memory-model.md`

Focus:
- Page size: 4K logical (x86) vs 16K Mach (Apple Silicon). Where misalignment can happen?
- mprotect / mach_vm_protect consistency. POSIX vs Mach semantics.
- View registry vs range registry — when do they diverge?
- WRITECOPY semantics на Apple Silicon (no CoW like Linux)
- Address space layout: x64 guest (preferred 0x140000000) vs native ARM64 PE (high 0x7ffd...) — collisions?
- Coalescing / splitting of VM regions

Specific check related to current Heisenbug:
- Find ALL call sites of mprotect(). For each: is return value checked? errno logged on failure?
- Find ALL call sites of mach_vm_protect(). Same check.
- Are there paths that bypass mprotect_exec()? Direct mmap with PROT_EXEC after our range registration?

### Dimension 2: Cross-arch boundary integrity
File: `docs/ENGINE-AUDIT-02-cross-arch-boundary.md`

Focus:
- Every transition from native ARM64 → guest x64 — does it go through trampoline?
- Every return from guest x64 → native ARM64 — preserve callee-saved (bug #6)?
- Signal handler chain: priority, fallback paths
- x18 preservation (bug #2)
- macOS sigreturn behavior

Specific: are there code paths where native ARM64 can be tricked into executing guest x64 bytes без signal trampoline? (This was bug #7 root, but is the fix complete?)

### Dimension 3: Decoder x64 coverage
File: `docs/ENGINE-AUDIT-03-decoder-coverage.md`

Focus:
- Intel SDM compliance: which opcodes are декодированы, which gaps remain
- Prefix handling: 0x66 (operand size), 0x67 (address size), REX, LOCK, REP/REPNZ, segment overrides
- AVX/VEX prefixes (almost certainly NOT implemented — confirm)
- 16-bit operand handling (bug #5 family)
- REX.W edge cases (bug #4 family)
- SSE↔GPR transfer family — check ALL 8 variants for REX.W respect

Output: matrix of opcodes coverage с known holes ranked by likelihood of MSVC compiler emitting them.

### Dimension 4: Lifter / IR semantic fidelity
File: `docs/ENGINE-AUDIT-04-lifter-ir.md`

Focus:
- Does lifter preserve x86 semantics fully?
- Flag computation: lazy vs eager — consistency
- Partial register write semantics (8/16-bit writes preserve upper, 32-bit zero-extends)
- Memory operand effective address calculation
- Branch target computation (RIP-relative, jumps, calls)

Reference bug #1 (RIP-relative + trailing immediate). Are similar arithmetic bugs lurking elsewhere?

### Dimension 5: JIT vs interpreter consistency
File: `docs/ENGINE-AUDIT-05-jit-interp-consistency.md`

Focus:
- Same IR should produce same effect through JIT and through interpreter
- Are there ops implemented in one but not other?
- Are there ops with different behavior between them?
- Bug #6 was JIT-only (NEG flags not updated); are there other JIT divergences?

Output: side-by-side audit of all IR ops, JIT impl, interp impl, marked equivalent / divergent.

### Dimension 6: Signal handling & race conditions
File: `docs/ENGINE-AUDIT-06-signal-races.md`

Focus:
- Primary handler installation timing (before/after Wine init)
- Handler chain mechanics (sigaction prev, chain calls)
- Concurrent fault delivery (multi-thread)
- Signal mask handling
- SA_NODEFER usage
- Signal during signal (nested)
- TLS access from signal handler

This is where current Heisenbug likely lives. Audit hard.

### Dimension 7: ABI marshalling (cross-arch)
File: `docs/ENGINE-AUDIT-07-abi-marshalling.md`

Focus:
- Argument register mapping x64 SysV → ARM64
- Stack argument shuffling
- Return value handling (single/multi/struct)
- SSE register transfers (bug #4)
- Variadic functions
- WinAPI thunks: argument decoding correctness
- ACCESS_MASK family — is there bug #5-like 16-bit truncation elsewhere?

### Dimension 8: Threading & TLS
File: `docs/ENGINE-AUDIT-08-threading.md`

Focus:
- Thread creation (CreateThread, RtlCreateUserThread)
- TLS slots (Windows TLS, FLS)
- DllMain chain on thread create/exit
- Locks: which subsystems use what lock?
- Lock ordering (deadlock potential)
- Reentrancy

### Dimension 9: Error handling & silent failures
File: `docs/ENGINE-AUDIT-09-silent-failures.md`

Focus:
- **Find every function that can fail silently** (returns void, return value ignored, error path not logged)
- mprotect return value handling
- mmap return value handling
- WinAPI return value propagation
- NTSTATUS conversion correctness
- Logging consistency: которые errors are LOGged vs swallowed

This is **directly related** to current Heisenbug — mprotect might be failing silently.

### Dimension 10: Test coverage
File: `docs/ENGINE-AUDIT-10-test-gaps.md`

Focus:
- Which fundamental bugs (#1-7) have regression tests? Which don't?
- Which opcode classes have tests for: trigger byte, sibling, edge cases?
- Are there end-to-end tests for cross-arch dispatch?
- Synthetic stress tests (random valid x64 → execute → verify) — exist?
- Property-based tests (decoder roundtrip, etc) — exist?
- Threading tests with race detection?

Output: matrix of coverage гaps prioritized by risk.

---

## 5. Specific Heisenbug focus (extra effort)

Spend 1-2 sessions specifically on current bug Codex is hitting:

**Symptom**: With `MACRUNNER_HB_TRACE_HOST_EXEC=1` → x64 .text properly no-exec → next blocker is different (pc=0x400000 native relay).
With `MACRUNNER_HB_TRACE_HOST_EXEC=0` → x64 .text shows PROT_EXEC restored → old FLS crash.

Investigation directions:

### A. Trace as Heisenbug
- Read `macrunner_hb_trace_host_exec` env handler. Does the very ACT of logging change timing enough to expose a race?
- Are there atomic ops missing where compiler reorders?
- Does fprintf in trace path act as memory barrier?

### B. Find all mprotect() and mach_vm_protect() call sites
- For each: under what condition called?
- Return value checked?
- Page alignment correct (16K)?
- Could two threads call concurrently on overlapping range?

### C. Find all PROT_EXEC restorers
Search for ANY code path that adds PROT_EXEC to memory:
- mprotect()
- mach_vm_protect()
- mmap() with PROT_EXEC
- vm_remap() preservation
- Wine internal protect paths

For x64 guest range, ALL of these should respect our no-host-exec policy. Find paths that don't.

### D. Audit signal handler restore semantics
- Does signal handler return path use sigreturn that restores VM protection?
- macOS specific: are there exception ports that bypass posix signals?
- darwin-specific exception_handler that runs before/after sigaction handler?

### E. Build a single hypothesis matrix
Output: `docs/ENGINE-AUDIT-HEISENBUG.md` с table:

| Hypothesis | Evidence for | Evidence against | Confidence | Suggested probe |
|---|---|---|---|---|
| mprotect silently fails | ... | ... | high/med/low | concrete probe Codex can run |
| Race between threads | ... | ... | ... | ... |
| ... | ... | ... | ... | ... |

Rank top 3 hypotheses. Codex tests them in order.

**This is most valuable single deliverable from audit.** Even if rest of audit takes weeks, this Heisenbug analysis could save Codex days.

---

## 6. Cross-validation with bugs #1-7

For each known fundamental bug, audit if the **fix is complete**:

### Bug #1 — RIP-relative + trailing immediate
- Check ALL opcodes with RIP-rel + imm: ADD/SUB/CMP/TEST/MOV/IMUL/BT etc
- Were all 12 affected forms patched?
- Any new opcodes added recently that have same pattern but missing fix?

### Bug #2 — x18 sigreturn
- Is x18 restored in ALL trampolines? (signal, callback, syscall, exception)
- Are there code paths that read x18 before our restoration?
- Bug #6 confirmed sibling — is the family fully covered?

### Bug #3 — Basic block PC fall-through
- Does ALL block dispatch advance PC after non-branch ends?
- JIT path same as interpreter?
- Are there block types that bypass the dispatcher?

### Bug #4 — REX.W on 0F 7E (MOVD vs MOVQ)
- Audit ENTIRE SSE↔GPR transfer family: 0F 6E, 0F 7E, 0F D6, F3 0F 7E, F2 0F D6 — each with all prefix combinations
- Are all REX.W cases respected?
- Are there other "ignored prefix" patterns?

### Bug #5 — 0x66 prefix on ALU group
- Audit ALL prefix sensitivity:
  - 0x66 — operand size (ADD/OR/ADC/SBB/AND/SUB/XOR/CMP confirmed; also TEST? INC/DEC? shifts?)
  - 0x67 — address size (LEA confirmed bug; what else?)
  - REP/REPNZ — string ops (audit MOVS, CMPS, SCAS, LODS, STOS)
  - LOCK — atomics
  - Segment overrides (FS/GS) — used for TLS

Find any opcode that has prefix-sensitive operand but ignores prefix.

### Bug #6 — ARM64 callee-saved across x64 callback
- Audit ALL cross-arch entry points (not just primary trampoline)
- Each must preserve x19-x28 + d8-d15
- Each must restore correctly on return
- TLS callbacks, FLS, APC, COM vtable thunks — all paths

### Bug #7 — x64 host-exec / silent ARM64 execution
- The CURRENT Heisenbug — covered in Section 5
- Beyond Heisenbug: are there other "silent failure" patterns? Bytes interpreted as different instruction set by accident?

---

## 7. Output format

### Per-dimension finding doc structure

```markdown
# ENGINE-AUDIT-NN — {Dimension}

Date: YYYY-MM-DD
Auditor: Kimi
Read-only audit. NO code changes by auditor.

## Scope reviewed
- Files: [list with line counts]
- Total lines audited: NNN

## Methodology
[How you analyzed — exhaustive read, grep-based pattern search, etc.]

## Findings

### Finding NN.1 — {brief title}
Severity: CRITICAL / HIGH / MEDIUM / LOW
Confidence: high / medium / low
File: path/to/file.c:LINE_NUMBER

**Symptom**: what manifests
**Root cause analysis**: what's wrong and why
**Reproduction**: how to verify (commands, env, expected outcome)
**Impact scope**: affects which scenarios, apps, paths
**Suggested fix scope**: where Codex should look (NOT the fix itself)

### Finding NN.2 — ...
...

## Patterns observed
[Recurring themes — e.g., "many places ignore mprotect return value", "no consistent error logging"]

## Recommendations
[Architectural improvements suggested for future]

## Things that look correct
[Validation — explicit list of patterns audited and found OK. Important: not everything is broken.]
```

### Cross-cutting summary

After all 10 dimensions done, write:

`docs/ENGINE-AUDIT-SUMMARY.md` with:
- Top 10 findings ranked by severity × confidence
- Identified bug classes not yet hit by any app
- Architectural recommendations
- Test coverage gaps prioritized

This is the deliverable Timur reads first.

---

## 8. Deliverables (by phase)

### Phase 1 (Sessions 1-3) — Heisenbug analysis (URGENT)

Codex needs this ASAP. Deliver before continuing other dimensions.

- [ ] `docs/ENGINE-AUDIT-HEISENBUG.md` с hypothesis matrix
- [ ] Top 3 hypotheses ranked with specific probe Codex can run
- [ ] If confident root cause identified — file `docs/ENGINE-AUDIT-FINDING-HEISENBUG.md` with full analysis

### Phase 2 (Sessions 4-10) — Dimensional audit

One dimension per session (or one dim spanning 2 sessions if needed):

- [ ] `docs/ENGINE-AUDIT-01-memory-model.md`
- [ ] `docs/ENGINE-AUDIT-02-cross-arch-boundary.md`
- [ ] `docs/ENGINE-AUDIT-03-decoder-coverage.md`
- [ ] `docs/ENGINE-AUDIT-04-lifter-ir.md`
- [ ] `docs/ENGINE-AUDIT-05-jit-interp-consistency.md`
- [ ] `docs/ENGINE-AUDIT-06-signal-races.md`
- [ ] `docs/ENGINE-AUDIT-07-abi-marshalling.md`
- [ ] `docs/ENGINE-AUDIT-08-threading.md`
- [ ] `docs/ENGINE-AUDIT-09-silent-failures.md`
- [ ] `docs/ENGINE-AUDIT-10-test-gaps.md`

### Phase 3 (Sessions 11-13) — Cross-validation

- [ ] `docs/ENGINE-AUDIT-BUGS-COMPLETENESS.md` — for each of 7 known bugs, is fix complete?
- [ ] Identified gaps / incomplete fix coverage filed as findings

### Phase 4 (Sessions 14-15) — Synthesis

- [ ] `docs/ENGINE-AUDIT-SUMMARY.md` — top 10 findings, recommendations
- [ ] `docs/ENGINE-AUDIT-METHODOLOGY.md` — how this audit was done, lessons for next audit

---

## 9. Methodology rules (same as your other work)

### Zeroth Principle applies
- Root-cause every finding, not just symptom
- Native macOS semantics consideration (Mach vs POSIX where different)
- No "I bet that's broken" without evidence

### Patch-by-evidence
- Every finding должна иметь reproducible evidence — log lines, line numbers, test cases
- Confidence rating is mandatory: don't claim high confidence без firm evidence
- Suggested fix scope only — не fix code yourself

### Decision autonomy
- You decide which finding is CRITICAL vs HIGH vs MEDIUM
- You prioritize ordering of dimensions if needed
- Escalate only:
  - Architectural rewrites (>1000 lines of suggested changes)
  - Conflicts с Codex's active work (e.g., finding contradicts его recent fix)
  - Discoveries that require strategic decisions (e.g., we should rewrite X subsystem)

### Process hygiene
- Read source carefully, don't skim — many bugs are subtle 5-character mistakes
- Cross-reference: when you see suspicious pattern, grep for similar patterns elsewhere
- Test only via existing test harness; don't write throwaway test programs

---

## 10. Communication

### Progress log: `docs/KIMI-PROGRESS-engine-audit.md`

```markdown
## YYYY-MM-DD — Phase N, Session M

What was audited:
- [files/dims covered]

Key findings this session:
- [bullet points]

Open questions to Codex (low priority — not blocking him):
- [questions if any]

Next session plan:
- [next dimension]
```

### Cross-talk with Codex

Avoid notifying Codex with each finding — он in deep focus on bug #7. Batch findings, deliver synthesis at end of each phase.

**Exception**: if you find the Heisenbug root cause in Phase 1, deliver IMMEDIATELY:
- File `docs/ENGINE-AUDIT-FINDING-HEISENBUG.md`
- Update progress log
- Notify Timur (he forwards to Codex)

This single finding может сэкономить Codex hours/days.

---

## 11. First-session goals

Session 1 — Heisenbug deep dive (HIGHEST PRIORITY):

1. Read AGENTS.md, 90-architectural-discoveries doc, и last 200 lines of Codex's recent journey context (latest commits, last few reports from /Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/phase-h/)
2. Read complete:
   - engine/wine/dlls/ntdll/unix/virtual.c (7721 lines)
   - engine/wine/dlls/ntdll/unix/signal_arm64.c (2841 lines)
   - engine/wine/dlls/ntdll/unix/macrunner_hb.c (3049 lines)
3. Specifically для Heisenbug:
   - Find all mprotect/mach_vm_protect call sites
   - Note return value handling for each
   - Note alignment for each
   - Identify any code that adds PROT_EXEC after image map
4. Build hypothesis matrix
5. If clear root cause emerges — file finding immediately

Session 2 — finalize Heisenbug if needed, begin Dimension 1 (memory model)

Session 3+ — proceed through dimensions

---

## 12. Timeline expectations

| Phase | Sessions | Days (est) |
|---|---|---|
| Phase 1 Heisenbug | 1-3 | 1-2 |
| Phase 2 Dimensions | 4-10 | 5-7 |
| Phase 3 Cross-validation | 11-13 | 2-3 |
| Phase 4 Synthesis | 14-15 | 1-2 |
| **Total** | **15** | **9-14 days** |

Run в parallel с networking brief (your other primary). Audit когда networking design phase is between ADRs / waiting on something. Or split days: morning audit, afternoon networking. You decide.

---

## 13. The value proposition

If audit finds:

- **Heisenbug root cause** — saves Codex days of narrow debug → main window faster → Phase H closes
- **One bug class пропущенный** in existing fixes — saves weeks reactive debug на KeePass/1С
- **Multiple class gaps** — proactively closed before next 5 apps hit them
- **Test coverage gaps** — surfaced before they bite production
- **Architectural improvements** — informs future refactoring

This audit is **highest leverage work** available right now. Networking is critical long-term, but audit is critical short-term.

---

## Appendix A — Path quick reference

```
Repo root:                  /Users/timurtoby/Documents/MacRunner/Main/MacRunner
HyperBridge core:           engine/hyperbridge/{src,include,tests}/
Wine ntdll Unix side:       engine/wine/dlls/ntdll/unix/
Wine PE ntdll:              engine/wine/dlls/ntdll/
kernelbase:                 engine/wine/dlls/kernelbase/
win32u:                     engine/wine/dlls/win32u/
Existing test scripts:      ./scripts/test-hyperbridge.sh, ./scripts/run-notepad-x64.sh
Recent run logs:            reports/phase-h/npp-x64-*/
Obsidian context:           /Users/timurtoby/Documents/MacRunner/90-architectural-discoveries-fundamental-bugs.md
                            /Users/timurtoby/Documents/MacRunner/91-notepad-plus-plus-journey.md
```

## Appendix B — Sanity check first

```bash
cd /Users/timurtoby/Documents/MacRunner/Main/MacRunner
. config/env.sh
echo "Root: $MACRUNNER_ROOT"

# Verify you can read all key files
wc -l engine/wine/dlls/ntdll/unix/virtual.c engine/wine/dlls/ntdll/unix/signal_arm64.c engine/wine/dlls/ntdll/unix/macrunner_hb.c
wc -l engine/hyperbridge/src/hb_decode_x64.c engine/hyperbridge/src/hb_arm64_codegen.c engine/hyperbridge/src/hb_interpreter.c engine/hyperbridge/src/hb_flags.c

# Verify tests pass baseline
./scripts/test-hyperbridge.sh 2>&1 | tail -10

# Find Codex's recent activity context
ls -t reports/phase-h/ | head -5
git log --oneline -20
```

Everything readable + baseline tests green = you're set.

---

## Appendix C — What this audit is NOT

- It is NOT performance audit (profiling, opt). Future work.
- It is NOT security audit (CVE-style). Future work.
- It is NOT code style / linting. Codex's territory.
- It is NOT documentation writing for users. Different task.
- It is NOT refactoring proposals. You may suggest architectural improvements but don't propose rewrites.

It IS: correctness audit. Find bugs, race conditions, silent failures, missed bug classes, test gaps.

---

End of brief. **You are auditor of MacRunner engine.**

This work has potential to save WEEKS of Codex reactive debug. Take it seriously. Read carefully. Document precisely.

Find what Codex can't see from inside the tunnel.
