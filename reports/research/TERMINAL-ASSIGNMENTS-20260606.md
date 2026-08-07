# MacRunner — Terminal / Model assignments (2026-06-06)

How many terminals, which model, which lane. Coordinator = Claude (this terminal).

## Active terminals (3 workers + coordinator)

### T0 — Claude (Opus thinking) — COORDINATOR  [you keep this one as me]
- Triage routing: after every lane run → `python3 tools/triage/classify_run.py <run-dir>` →
  route `OWNER` finding to the lane, seed its next prompt from `NEXT_ACTION` + `NEXT_RUN_ENV`.
- Lane A "eyes": map pc `0x10B44F015` → module/RVA, classify SEH/ARM64EC boundary, feed Codex.
- Merges, `AGENT-TEAM-OWNERSHIP.md`, `CODEX-MEGA-PROGRAM-status.md`, memory.

### T1 — Codex (xhigh) — LANE A (engine/JIT → window) — PRIORITY 1, critical path
- Drive Hollow Knight + AI War 2 past `GFXDEVICE_SEH_HOST_BOUNDARY_BEFORE_D3D11` to `D3D11CreateDevice`.
- Immediate: one bounded HK run 420s with the new `macrunner-hb-seh-host-boundary-detail` marker
  → `classify_run.py` → with Coordinator, map pc→RVA, get side/x64_rip/exc-code/ldr_status/resume.
- Owns `hb_*`, `macrunner_hb.c`, `signal_arm64.c`, `mr-run.sh`. Mission: `LANE-A-MEGA-MISSION.md`.
- Pairing: Codex = hands (build/run/fix), Claude(T0) = eyes (triage/RVA/classify).

### T2 — Claude (Opus thinking) — LANE PE32/WOW64 (32-bit) — PRIORITY 2, separate front
- Goal: PE32 Notepad++ → `BTCpuProcessInit`/`BTCpuThreadInit`/`BTCpuSimulate`, then window.
- Gate now: `PE32_WOW64CPU_NOT_LOADED` — wow64cpu/xtajit/i386-ntdll handoff never starts BTCpu.
- Owns `xtajit/**` (32-bit), `wow64`, `wow64cpu`, `wow64win`. Mission: `LANE-PE32-WOW64-MISSION.md`.
- HARD: never touch `xtajit64`, `hb_*`, `macrunner_hb.c`, `signal_arm64.c` (Lane A) → request via T0.
- Why Opus not Codex: deep engine-debug + Codex is busy on Lane A. (Codex xhigh also fine if free.)

### T3 — Gemini — LANE E (research + product + prep) — NON-ENGINE only
- AI War 2 (GOG x64 Unity) compat research: installer type (Inno/GOG), DRM, DX version, Unity ver.
- Lane F/G prep docs (Diablo1 DDraw oracle, GTA VC D3D8 smoke plan, installer/FreeArc checklist).
- Optional: deepen `app/configurator/exe_analyzer.py` (anti-cheat sig DB, engine fingerprint,
  dynamic-import scan) + merge with `pe_analyzer`; design triage generic-fallback analyzer.
- HARD: **never edit engine code** (`engine/**`, `hb_*`, ntdll). Research/docs/`app/configurator/**` only.

## Standby lanes (activate on triage OWNER, do NOT pre-spin)
- **Lane B (Kimi)** — ISA/decode/interp. Activate when triage emits unsupported-opcode / bad-flags /
  wrong-JIT-helper / bad-memory / bad-atomics from A or PE32. Currently green (458/0), isolated kit.
- **Lane C (Codex 2nd)** — Win32 API/loader breadth. Activate on missing-import/loader/DLL/API fail.
- **Lane D (Codex/Kimi)** — DXMT graphics. Activate when A reaches `D3D11CreateDevice` OR x86_64-unix
  Wine artifacts restored (`x86_64-unix/{winemac.so,ntdll.so}` missing today).

## The loop (every run, every lane)
```
run → run.log + flight JSONL
  → python3 tools/triage/classify_run.py <run-dir>      # MANDATORY GATE (Lane X)
  → read OWNER + CLASS + CONFIDENCE + NEXT_ACTION + NEXT_RUN_ENV
  → Coordinator routes to OWNER lane, seeds next prompt from NEXT_ACTION
```

## Model law
- **Engine semantics (A / PE32 / B-core): Codex xhigh OR Claude Opus thinking ONLY.**
- **Gemini/Flash (agy/Antigravity): research, prep docs, product Python — NEVER engine code.**
  HAS context-mode (full ctx toolset via `~/.gemini/config/mcp_config.json`) — tell it in-terminal to use ctx.
  Separate account (`ljyudith9@gmail.com`), default CONTEXT_MODE_DIR (run in same repo cwd for shared KB).
  Interactive TUI only (no clean headless exec) → operator-driven, not coordinator-spawned.
- One critical path (window) = one agent on Lane A; extra agents build breadth, not speed.

## Effort / power tier (Claude effort slider: Faster ← … → Extra → Ultracode = max "Smarter")
Coordinator picks per task. Ultracode is slow + token-heavy → reserve for the hard nodes.
| Effort | Use on |
|--------|--------|
| **Ultracode** (max) | hardest root-cause: Lane A SEH host-boundary, PE32 BTCpu handoff, any novel blocker |
| **Extra / xhigh** | normal engine-debug, pc→RVA mapping, failure classification |
| **high** | merges, triage analysis, coordination |
| **medium / low** | mechanical: builds, runs, zombie cleanup, routine |

## Token waterfall (never idle)
Lane A engine: **Codex 5.5** (`codex exec`, long non-stop task — finishes even on token-empty) →
**Codex 5.3 spark** → my background **`claude -p` Opus (Ultracode/xhigh)**.
Research/prep: **Gemini/agy** (operator-driven) → my **`claude -p` Sonnet**.
(No local fallback — ollama/llama exhausted, removed from chain.)

## Why 3 active, not more
Coordinator scaling limit ~4–5 lanes. Right now only 2 fronts are truly unblocked-and-moving
(Lane A window, PE32 BTCpu). Gemini fills non-engine breadth. B/C/D wake on demand via triage.
Spinning more engine terminals on the single critical path wastes them and risks merge collisions.
