# Kimi Master Brief #2 — AOT Translation Cache (audit, extend, integrate)

**Target agent**: Kimi
**Workstream**: Parallel to Codex Phase H bug #7 closure
**Duration estimate**: 1-2 weeks focused work
**Conflict risk with Codex**: MEDIUM (will need engine/hyperbridge/ touches in Phase 2; coordinate timing)

---

## 0. Read first (mandatory)

1. [/Volumes/MacOS/MacRunner/AGENTS.md](../AGENTS.md) — особенно **🎯 Zeroth Principle** (native, root-cause, seamless) и mandatory protocols
2. [/Volumes/MacOS/MacRunner/docs/KIMI-TASK-audio-stack-master-brief.md](KIMI-TASK-audio-stack-master-brief.md) — твой первый brief, формат и стиль работы остаются те же
3. This brief

If you completed audio stack Phase 1+2 (12/12 tasks) — отлично. Эта задача following the same methodology: design first, evidence-driven, family fixes, decision autonomy.

---

## 1. Context — what AOT cache is and why it matters

**HyperBridge** компилирует x86_64 basic blocks → ARM64 machine code dynamically (JIT). Каждый раз когда app запускается — те же блоки компилируются заново. Notepad++ может стартовать 5-10s из-за JIT overhead. Productivity apps (AutoCAD, Photoshop, Revit) будут стартовать ещё медленнее.

**AOT cache** persists JIT output на disk. Next run загружает pre-compiled blocks вместо recompile. UX impact:

- Cold start (no cache): 5-10s (JIT всё)
- Warm start (cache hit): 0.5-1s (load + link only)
- Speedup: **5-10x**

Это **visible quality bump** для пользователя — критично для премиум продукта.

---

## 2. Current state — what exists

AOT cache **partially implemented** в codebase:

| File | Status | Lines |
|---|---|---|
| `engine/hyperbridge/include/hb_cache.h` | Public API header — keys, entries, format | 85 |
| `engine/hyperbridge/src/hb_aot_cache.c` | Implementation — file I/O, hashing, lookup | 389 |
| `engine/hyperbridge/tests/hb_test_runner.c` | Tests `aot_cache_roundtrip`, `translation_cache_api_stats_and_module_invalidate` | (in 9000+ line file) |

**Что работает**:
- File format (magic bytes, versioning, ABI versioning)
- Atomic writes (temp file + fsync + rename — readers never see partial)
- Cache key computation (code hash, module_id, arch, mode, backend, flags)
- Roundtrip tests (put → get → invalidate → clear) passing

**Чего НЕТ (твоя работа)**:

1. **Integration в JIT path** — `hb_jit.c` и `hb_arm64_codegen.c` НЕ используют cache. JIT всегда compile from scratch.
2. **Cache directory management** — где cache живёт? `~/Library/Caches/MacRunner/`? Кто создаёт?
3. **Module identity policy** — `module_id` сейчас abstract; нужна реальная stable hash strategy (PE checksum? path? bytes?)
4. **Eviction / size limits** — cache grows unbounded
5. **Cross-process sharing** — если две apps запускают тот же DLL, должны share cache entries
6. **Invalidation triggers** — HyperBridge version bump? PE module modification? Manual purge command?
7. **Performance benchmarks** — actual measurement cold vs warm start
8. **Validation suite** — edge cases (corrupt file, partial write recovery, concurrent access)

---

## 3. Task — three phases

### Phase 1: Audit + standalone extensions (week 1) — **NO Codex coordination needed**

Pure work in `engine/hyperbridge/cache/` and `engine/hyperbridge/include/` for new public API. Doesn't touch JIT yet.

1. **Audit existing implementation** thoroughly:
   - Read `hb_cache.h` + `hb_aot_cache.c` end-to-end
   - Identify edge cases not covered (concurrent writes, corrupt files, partial reads, huge entries)
   - Document findings в `docs/AOT-CACHE-AUDIT.md`
   
2. **Cache directory layout standard**:
   - Define canonical path: `~/Library/Caches/MacRunner/hyperbridge/`
   - Per-module subdirs: `{module_hash}/` containing `{block_hash}.bin` files
   - Index file: `index.bin` listing valid entries with timestamps
   - Helper API `hb_cache_default_dir()` returns expanded path
   - Auto-create on first use
   
3. **Module identity policy**:
   - Decide: hash full module bytes? PE checksum field? path-based? combination?
   - Implement `hb_cache_compute_module_id(const void *pe_image, size_t size)`
   - Stable across runs of same exact binary, different для modified binaries
   - Document trade-offs in `docs/AOT-CACHE-DESIGN.md`
   
4. **Eviction policy**:
   - Size limit (default: 500MB)
   - LRU eviction when limit hit
   - API: `hb_cache_set_size_limit(cache, bytes)`, `hb_cache_evict_to_limit(cache)`
   - Unit test
   
5. **Validation suite** — extend tests:
   - Corrupt file recovery (truncate, bit-flip, magic byte tamper)
   - Concurrent access (two processes putting + getting)
   - Huge entries (10MB+ code blob)
   - Cache directory missing / read-only / disk full
   - Version mismatch handling (older format)
   
6. **Performance benchmarks standalone**:
   - Microbenchmark: put 1000 entries + retrieve 1000 entries
   - Latency: average lookup time
   - Throughput: entries/sec sustained
   - Disk usage: bytes per entry on disk
   - Document в `docs/AOT-CACHE-BENCHMARKS.md`

### Phase 2: Integration design (mid-week) — **PLAN ONLY, no integration code yet**

Coordinate timing with Codex. While Codex finishes bug #7, you design:

1. **Integration plan** в `docs/AOT-CACHE-INTEGRATION-DESIGN.md`:
   - Where in JIT path lookup happens (before/after `hb_arm64_codegen_block`)
   - How miss → compile → put cycle works
   - Thread safety (JIT can be invoked from multiple threads)
   - Failure modes (cache corrupted mid-run, disk full, permissions)
   - Rollback strategy (if integration causes regressions, switch off via env)
   
2. **Env knob**: `MACRUNNER_HB_AOT_CACHE` — values: `off` (default initially), `read` (lookup but no write), `write` (lookup + write), `purge-on-start`
   
3. **Identify required Codex coordination**:
   - List exact functions/files Codex's bug #7 might touch
   - Propose merge window after bug #7 closes
   - Pre-write integration tests that don't require live integration
   
4. **Submit design for review** — add entry в Questions for Timur log if blocking decisions

### Phase 3: Integration implementation (week 2+) — AFTER Codex Phase H closure

Only proceed when:
- Bug #7 closed (Notepad++ main window visible)
- Codex confirms Phase H stable
- Approved design doc

Then:
1. Wire `hb_cache_get` before `hb_arm64_codegen_block`
2. On hit: skip codegen, link cached code
3. On miss: codegen as before, then `hb_cache_put`
4. Add benchmark: cold vs warm Notepad++ start times
5. Regression test: full HyperBridge test suite still passes with cache on
6. Real-app validation: 5+ run Notepad++ with cache, measure speedup
7. Update `docs/AOT-CACHE-VALIDATION.md` с numbers

### Phase 4 (stretch, after Phase 3 success): Advanced

- **Prefetch on app launch**: walk PE imports, pre-load cache entries
- **Cross-process sharing**: shared memory or shared file, multiple Wine instances read same cache
- **Cloud cache**: download pre-built cache from server for popular apps (1С, AutoCAD)
- **Profile-guided optimization**: track hot blocks, prioritize them

---

## 4. Out of scope (do NOT do)

- Changes to `hb_arm64_codegen.c` JIT logic itself — codegen output must remain identical
- Changes to `hb_interpreter.c` — interpreter independent of cache
- Changes to HyperBridge IR (`hb_ir.h`, `hb_ir.c`)
- Changes to decoder (`hb_decode_x64.c`) — Codex's territory
- Anything в `engine/wine/dlls/ntdll/` or `engine/wine/dlls/kernelbase/` — Codex's territory
- New JIT backends (ARM64ec etc) — orthogonal work item

---

## 5. Reference materials

### Existing code (read but don't modify in Phase 1)

- `engine/hyperbridge/include/hb_cache.h` — public API
- `engine/hyperbridge/src/hb_aot_cache.c` — implementation
- `engine/hyperbridge/src/hb_arm64_codegen.c` — current JIT codegen (where integration will hook)
- `engine/hyperbridge/src/hb_jit.c` — JIT orchestrator
- `engine/hyperbridge/include/hb_codegen.h` — codegen types
- `engine/hyperbridge/include/hb_ir.h` — IR for understanding blob format

### Tests for style reference

- `engine/hyperbridge/tests/hb_test_runner.c` lines 1443-1500 — `aot_cache_roundtrip`
- `engine/hyperbridge/tests/hb_test_runner.c` line 1630+ — `translation_cache_api_stats_and_module_invalidate`

### macOS Cache directory conventions

- `~/Library/Caches/{bundle-id}/` — proper Apple guidelines location
- `xattr com.apple.metadata:kMDItemUserTags` — can tag for backup exclusion
- `sysdir_start_search_path_enumeration(SYSDIR_DIRECTORY_CACHES, SYSDIR_DOMAIN_MASK_USER)` — programmatic resolution

---

## 6. Methodology rules (from AGENTS.md, applicable here)

### Zeroth principle: native, root-cause, seamless

- Native macOS API for cache dir resolution, не hard-coded paths
- Root-cause for every found edge case в audit, не band-aid
- Atomic file operations only (already mostly done — extend if gaps found)

### Patch-by-evidence (mandatory)

- Before "fixing" existing code, demonstrate the bug exists via test
- Audit findings should have reproducible cases в commit messages

### Family audit

- If you find one race condition в cache code — audit ВСЕ I/O paths for similar issue
- If you find one off-by-one в hash — audit all hash callers

### Decision autonomy

Per AGENTS.md "Decision rubric" — common dilemmas have default answers:

- "Should cache live in ~/Library or /tmp?" → Library (proper Apple practice)
- "Should I add SHA-256 or use existing хеш?" → existing if it's not obviously broken
- "Should I support older Wine versions?" → No, MacRunner-only consumer
- "Should I add per-block compression?" → Only if size benchmark justifies; document with numbers

Don't block waiting for confirmation on reasonable choices. Document decision в design doc.

---

## 7. Workspace

### Where to write

```
/Volumes/MacOS/MacRunner/
├── engine/hyperbridge/cache/                      # NEW DIR for your extension work
│   ├── hb_cache_dir.c                              # default dir resolver
│   ├── hb_cache_dir.h
│   ├── hb_cache_module_id.c                        # module identity hashing
│   ├── hb_cache_module_id.h
│   ├── hb_cache_eviction.c                         # LRU eviction
│   └── hb_cache_eviction.h
├── engine/hyperbridge/include/                    # ADD headers here
│   └── hb_cache_extensions.h                       # your new public API
├── engine/hyperbridge/tests/                      # ADD tests
│   └── hb_test_aot_cache_ext.c                     # standalone tests for your work
├── docs/                                           # ADD documentation
│   ├── AOT-CACHE-AUDIT.md                          # Phase 1 audit findings
│   ├── AOT-CACHE-DESIGN.md                         # decisions, format, layout
│   ├── AOT-CACHE-BENCHMARKS.md                     # Phase 1 perf
│   ├── AOT-CACHE-INTEGRATION-DESIGN.md             # Phase 2 plan
│   ├── AOT-CACHE-VALIDATION.md                     # Phase 3 results
│   └── KIMI-PROGRESS-aot.md                        # YOUR PROGRESS LOG
```

### Do NOT modify (these are Codex's active areas or shared infrastructure)

- `engine/hyperbridge/src/hb_aot_cache.c` — existing implementation; extend via new files, don't rewrite
- `engine/hyperbridge/include/hb_cache.h` — existing public API; add extensions in separate header
- `engine/hyperbridge/src/hb_arm64_codegen.c` — JIT codegen (Phase 3 only, after Codex sign-off)
- `engine/hyperbridge/src/hb_jit.c` — JIT orchestrator (Phase 3 only)
- `engine/hyperbridge/src/hb_decode_x64.c` — decoder (Codex territory)
- `engine/wine/dlls/ntdll/` — entirely Codex
- `engine/wine/dlls/kernelbase/`, `win32u/` — Codex

If you need to integrate с existing `hb_aot_cache.c` API — call its public functions from your new code. Don't modify it.

### Build integration

Add new sources to HyperBridge build:
- Modify `engine/hyperbridge/Makefile` или CMakeLists carefully — append, не rewrite
- OR: standalone `scripts/build-hyperbridge-cache-ext.sh` (same pattern as audio)
- Make sure `./scripts/test-hyperbridge.sh` picks up your tests

---

## 8. Deliverables checklist

### Phase 1 (week 1)
- [ ] `docs/AOT-CACHE-AUDIT.md` — findings from reading existing impl
- [ ] `docs/AOT-CACHE-DESIGN.md` — your design decisions for extensions
- [ ] `engine/hyperbridge/cache/hb_cache_dir.{c,h}` — directory resolver
- [ ] `engine/hyperbridge/cache/hb_cache_module_id.{c,h}` — module identity
- [ ] `engine/hyperbridge/cache/hb_cache_eviction.{c,h}` — LRU eviction
- [ ] `engine/hyperbridge/tests/hb_test_aot_cache_ext.c` — comprehensive tests including edge cases
- [ ] `docs/AOT-CACHE-BENCHMARKS.md` — standalone perf numbers
- [ ] `docs/KIMI-PROGRESS-aot.md` updated each session

### Phase 2 (mid-week)
- [ ] `docs/AOT-CACHE-INTEGRATION-DESIGN.md` — integration plan
- [ ] Pre-written integration tests (mock JIT path)
- [ ] Questions for Timur logged if needed

### Phase 3 (week 2+, AFTER Codex Phase H closure)
- [ ] Integration commits in `hb_jit.c` / `hb_arm64_codegen.c`
- [ ] `docs/AOT-CACHE-VALIDATION.md` — Notepad++ cold/warm benchmarks
- [ ] Regression: full HyperBridge test suite green
- [ ] `MACRUNNER_HB_AOT_CACHE` env knob документировано в AGENTS.md

### Phase 4 (stretch, only if Phase 3 solid)
- [ ] Prefetch on app launch
- [ ] Cross-process sharing

---

## 9. Success criteria

- All Phase 1 tests pass (existing + new edge cases)
- Phase 1 perf: cache lookup < 1ms p99, cache put < 5ms p99
- Phase 3 (after integration): Notepad++ warm start **at least 3x faster** than cold start
- Zero regressions в HyperBridge core tests
- All commits have proper family audit checklist в message
- Documentation полная: design + audit + benchmarks + validation

---

## 10. Communication (same pattern as audio)

`docs/KIMI-PROGRESS-aot.md` per session:

```markdown
## YYYY-MM-DD

What was done today:
- [bullet points]

Decisions made (per autonomy):
- [bullet points с rationale]

Blockers / questions:
- [bullet points] (only TRUE blockers; reasonable choices — decide yourself)

Next session plan:
- [bullet points]
```

If unexpected overlap with Codex's areas — STOP, log в questions, don't modify shared code without confirmation.

---

## 11. First-session goals

Like audio brief — design first, code in session 2.

Session 1 (read + design):
1. Read AGENTS.md (especially Zeroth Principle if not done)
2. Read existing `hb_cache.h` + `hb_aot_cache.c` end-to-end
3. Read JIT integration points (`hb_jit.c`, `hb_arm64_codegen.c`) для понимания where Phase 3 will hook
4. Create `docs/AOT-CACHE-AUDIT.md` skeleton with initial findings:
   - What's well-designed
   - What's incomplete
   - What edge cases concern you
5. Create `docs/KIMI-PROGRESS-aot.md` with first entry
6. **NO code yet** — design must finalize first

Session 2 (start coding):
1. Cache directory resolver (smallest standalone piece)
2. Tests for it
3. Update progress log

Cycle: design → impl → tests → progress entry. Same disciplined rhythm as audio.

---

## Appendix A — Path quick reference

```
Repo root:          /Volumes/MacOS/MacRunner
Your workspace:     engine/hyperbridge/cache/  (CREATE — new)
Existing AOT:       engine/hyperbridge/src/hb_aot_cache.c  (READ ONLY)
Existing AOT header: engine/hyperbridge/include/hb_cache.h  (READ ONLY)
JIT integration:    engine/hyperbridge/src/hb_jit.c  (Phase 3 ONLY)
Test runner:        engine/hyperbridge/tests/hb_test_runner.c  (extend with new test file)
Test script:        ./scripts/test-hyperbridge.sh
Progress log:       docs/KIMI-PROGRESS-aot.md
```

---

## Appendix B — Sanity check first command

```bash
cd /Volumes/MacOS/MacRunner
. config/env.sh
echo "Root: $MACRUNNER_ROOT"
ls engine/hyperbridge/include/hb_cache.h
ls engine/hyperbridge/src/hb_aot_cache.c
wc -l engine/hyperbridge/src/hb_aot_cache.c  # should be ~389
grep -c 'TEST(' engine/hyperbridge/tests/hb_test_runner.c  # count tests
./scripts/test-hyperbridge.sh  # verify baseline green before any changes
```

If all this works — ты в правильном месте, можно начинать.

---

End of brief. Welcome to AOT cache work. Apply Zeroth Principle throughout.
