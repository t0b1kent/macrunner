# LANE C — STANDING MEGA-MISSION: Win32/NT loader + memory + Win32-DLL API breadth

You are **Lane C**. This is a **months-long standing mission**, NOT a checklist you finish.
It is "done" ONLY when the COMPLETION CRITERIA at the bottom are ALL met. After you finish ANY
item, **immediately pick the next item from the BACKLOG and keep going** — do NOT stop, do NOT
declare the mission complete, do NOT wait for a new prompt. The only valid stops are the three
STOP conditions at the very end.

Read `reports/research/AGENT-TEAM-OWNERSHIP.md` (the law).

## OWNED FILES (edit ONLY these)
- `engine/wine/dlls/ntdll/unix/loader.c`, `virtual.c`
- `engine/wine/dlls/**` EXCEPT `ntdll/unix/macrunner_hb.c` and `ntdll/unix/signal_arm64.c`
  (those are Lane A — if you need a thunk/API handled there, RECORD it in
  `reports/research/LANE-C-API-GAPS.md` for Lane A; do NOT edit it).
**NEVER touch:** macrunner_hb.c, signal_arm64.c (Lane A), engine/hyperbridge/** (Lane A/B),
engine/dxmt|vkd3d|graphics (Lane D), the golden x64 snapshot.

## COMPLETION CRITERIA (the mission is NOT done until ALL hold)
1. A representative set of real x64 + x86 Windows programs (a CLI tool, a GUI app, and ≥3 games
   across engines) all LOAD and reach their entry/init without loader/memory/API faults
   attributable to your layer (`UNSUPPORTED`/wrong-result in loader/virtual/Win32 DLLs = 0 for the
   sampled set).
2. PE-loader handles every edge in the backlog (TLS, delay/bound imports, forwarded exports, SxS,
   relocations, LAA, exotic section layouts) with evidence.
3. VM/memory semantics (`virtual.c`) match Windows for the cases real software relies on, including
   JIT executable pages and WOW64 address-space edges.
4. `LANE-C-API-GAPS.md` is a living, triaged ledger; every gap is either fixed (in a real DLL) or
   classified (privileged / Lane-A-thunk / out-of-scope) with rationale.
This is a moving target (the Win32 surface is huge) — treat "done" as asymptotic; keep closing gaps.

## BACKLOG (work top-to-bottom; always have a next item; loop forever until COMPLETION)
### Phase 1 — PE loader robustness (`loader.c`)
- TLS callbacks (multiple, ordering), delay-load imports, bound imports, forwarded exports,
  API-set/apiset resolution, SxS/manifest activation contexts, relocation corner cases,
  large-address-aware, unusual section alignment/permissions, in-memory vs mapped images,
  resource section access. Each fix: a program/PE that failed now loads — paste evidence.
### Phase 2 — Memory / VM (`virtual.c`)
- `NtAllocateVirtualMemory`/`Protect`/`Query`/`Free` semantics, MEM_RESET/RESET_UNDO, guard pages,
  large/huge pages, placeholder/`MEM_REPLACE_PLACEHOLDER`, section mapping, copy-on-write,
  executable-page lifecycle for the JIT, WOW64 4GB address-space edges, stack growth/guard.
### Phase 3 — Win32 DLL API breadth (`engine/wine/dlls/**`, NOT the thunk shim) — REACTIVE
- Drive by running candidate programs; log each `UNSUPPORTED`/wrong-result API; implement/fix the
  REAL DLL (kernel32/kernelbase, user32, advapi32, ole32/combase, shell32, gdi32, ws2_32, etc.).
- Maintain `LANE-C-API-GAPS.md`: API → which DLL → status (fixed/privileged/Lane-A-thunk/oob).
### Phase 4 — Second/third game bring-up (surface gaps in parallel)
- Bring up another Unity title, then an Unreal title, then a non-engine app — each surfaces a fresh
  batch of loader/memory/API gaps. Feed them back into Phases 1–3. This proves "first game costly,
  later cheap" per engine.
### Phase 5 — 32-bit / WOW64 + D3D8/9-era prep (for older games e.g. GTA Vice City later)
- Harden the WOW64 loader/memory path for 32-bit guests; note D3D8/9 needs for Lane D.
### Phase 6 — Regressions & hardening
- For every fix, leave a check (test or a documented repro) so it can't silently regress.

## DISCIPLINE
- Every guest run via `scripts/mr-run.sh` + `timeout`; `scripts/mr-clean.sh --prune` after; NEVER
  global `pkill wine` (scoped `wineserver -k` only). **Stagger game runs with Lane A** (shared
  prefix collides) or use a separate WINEPREFIX. `./scripts/disk-guard.sh` before long cycles.
- Never read full trace logs (bound output). Evidence over status. NEVER `git add -A` — commit only
  your named files as `checkpoint(Lane C): ...`.

## STOP conditions (the ONLY reasons to stop — otherwise keep looping the backlog)
1. ALL completion criteria met (report it).
2. A hard external blocker after 3 distinct attempts (evidence + the decision you need).
3. An operator decision required (real trade-off). Anything else → next backlog item, keep going.
