# MacRunner — Agent Team & File-Ownership Matrix (THE LAW; updated 2026-06-06)

Multiple agents work in parallel. The ONLY thing that prevents collisions/merge-hell is strict
file ownership. **An agent edits ONLY files in its own lane. If it needs a change in another
lane's file, it notes it for that lane / the coordinator — it does NOT edit it.** Claude
(coordinator) does all merges and resolves conflicts. This file is the single source of truth;
when ownership changes, update HERE first.

## TL;DR — who runs where right now (2026-06-06)

| Lane | Owner model | Terminal | Status | Current gate |
|------|-------------|----------|--------|--------------|
| **A** Engine/JIT → window | **Codex xhigh** (hands) + Claude (eyes) | T1 (active) | 🔴 CRITICAL PATH | `GFXDEVICE_SEH_HOST_BOUNDARY_BEFORE_D3D11` pc=0x10B44F015 |
| **PE32** WOW64/xtajit 32-bit | **Claude Opus (thinking)** | T2 (active) | 🟠 NEW FRONT | `PE32_WOW64CPU_NOT_LOADED` (before BTCpuProcessInit) |
| **X** Flight Recorder / triage | Coordinator-owned, **all lanes use it** | — (gate, not a worker) | 🟢 READY | mandatory after every run |
| **B** ISA/decode/interp | **Kimi** | standby | 🟢 green (458/0) | waits for opcode/flags/atomics blocker from A |
| **C** Win32 API/loader breadth | Codex (2nd, on-demand) | standby | 🟢 clean | waits for missing-import/loader blocker |
| **D** Graphics/DXMT | Codex/Kimi | standby | 🟢 owned-green | waits for A to reach D3D11CreateDevice OR x86_64-unix artifacts |
| **E** Research (no code) | **Gemini / ChatGPT** | T3 (active) | 🟢 | AI War 2 compat, Lane F/G prep, triage fallback |
| **F/G** Legacy engines / installers | — | prep only | ⚪ blocked on PE32 | docs/oracle only until PE32 hits BTCpuSimulate |
| **Coordinator** | **Claude** (this terminal) | T0 | — | triage routing, merges, matrix, status, pc→RVA for A |

**Model rule (hard):** engine semantics (Lane A / PE32 / B core) = **Codex xhigh or Claude Opus
thinking only**. **NEVER Gemini/Flash on engine code** — Gemini does research/prep/product/triage
tooling only.

**Critical-path truth:** the window (Mono→GfxDevice→D3D11→pixels) is a SINGLE path — only Lane A
advances it; more agents ≠ faster. B/C/D/E build breadth in parallel so A isn't blocked when it
arrives. Coordinator (Claude) is the scaling limit (~4–5 active lanes).

---

## Lanes & owned files

### Lane A — Engine / JIT / throughput (critical path to the window) — Codex xhigh + Claude eyes
- `engine/hyperbridge/src/hb_arm64_codegen.c`, `hb_jit*.c`, `hb_runtime.c`, `hb_aot_cache.c`
- `engine/wine/dlls/ntdll/unix/macrunner_hb.c`  ← Lane A is ACTIVE here (thunks, context, heartbeat)
- `engine/wine/dlls/ntdll/unix/signal_arm64.c`
- `scripts/mr-run.sh`, `scripts/mr-clean.sh`
- Mission: `reports/research/LANE-A-MEGA-MISSION.md`. Focus: drive Hollow Knight + AI War 2 to a frame.
- **Current gate:** `GFXDEVICE_SEH_HOST_BOUNDARY_BEFORE_D3D11`, pc=`0x10B44F015` (non-LDR host
  region → HyperBridge generated host code / thunk boundary), hit BEFORE `D3D11CreateDevice`.
  Done: THREAD_DESKTOP_ZERO fixed, UnityWndClass CreateWindow passes, server_error=0, window handle
  created, GfxDevice reached. Next: one bounded HK run 420s with the new
  `macrunner-hb-seh-host-boundary-detail` marker → `classify_run.py` → map pc→module/RVA, get
  side / x64_rip / exception code / ldr_status / resume policy.
- Pairing (note-109 proven): **Codex = hands** (build/run/fix engine code), **Claude = eyes**
  (read triage/.ips, map pc→RVA, classify SEH/EC boundary, feed Codex). Same lane, two roles.

### Lane PE32 — WOW64 / xtajit / i386 32-bit path — Claude Opus (thinking)  [NEW, was operator's]
- `engine/wine/dlls/xtajit/**` (the **32-bit** module — NOT `xtajit64`, that's Lane A's x64 path)
- `engine/wine/dlls/wow64`, `wow64cpu`, `wow64win`
- WOW64 mirror bits already touched: `gdi32/gdiobj.c` peb32 GdiSharedHandleTable mirror.
- Mission: `reports/research/LANE-PE32-WOW64-MISSION.md`. Brief: `docs/CONTINUE-PE32-qsi-class102-hotspin-master-brief.md`.
- **Current gate:** `PE32_WOW64CPU_NOT_LOADED` (Lane X conf 0.95, PATCH_SAFE_NOW=yes). PE32 NPP
  lives, no crash/opcode-fault, BUT never reaches `BTCpuProcessInit`/`BTCpuThreadInit`/`BTCpuSimulate`
  → the i386 CPU never starts. Problem is BEFORE CPU exec: wow64cpu / xtajit / i386-ntdll handoff.
  Run: `reports/phase-h/npp-x86-diagnostic-20260606-150550`.
- **NOT Lane B.** This is not ISA — no opcode runs until BTCpu starts. Don't chase opcodes here.
- **HARD: never touch `xtajit64/**`, `hb_*`, `macrunner_hb.c`, `signal_arm64.c` (Lane A).** If a
  change is needed there → request via Coordinator.

### Lane X — Flight Recorder + Triage analyzer — Coordinator-owned, USED BY ALL LANES
- `tools/triage/**` (`classify_run.py` dispatcher; `analyze_{window_gate,d3d_gate,pe32_wow64_handoff,
  smc,arm64ec_callbacks,waits}.py`; `triage_common.py`; `regression_tracker.py`; `fixtures/**`)
- Spec: `reports/research/MACRUNNER-FLIGHT-RECORDER-SPEC.md`.
- **MANDATORY GATE:** after EVERY run, before the next prompt:
  `python3 tools/triage/classify_run.py <run-dir>` → read `OWNER` (=lane) + `CLASS` + `CONFIDENCE`
  + `NEXT_ACTION` + `NEXT_RUN_ENV`. The `OWNER` field auto-routes the failure to the right lane.
- Anti-fake-PASS guard is the point: downgrades to UNKNOWN if domain markers absent (note-100 law).
- Only improve on a real false-positive; do NOT gold-plate. Open improvement: a generic
  "unknown failure → cluster+summarize" fallback so novel failures get a hint, not bare UNKNOWN.

### Lane B — ISA / decode / interpreter (isolated kit) — Kimi  [STANDBY]
- `engine/hyperbridge/src/hb_decode_x64.c`, `hb_decode_x86.c`, `hb_lift_x64.c`, `hb_lift_x86.c`,
  `hb_interpreter.c`
- `engine/hyperbridge/include/hb_ir.h`, `hb_decoder.h`, `hb_context.h`
- `engine/hyperbridge/tests/**` (test_runner, coverage harnesses, fuzzer), `tools/hb_oracle/**`
- Works in `_air-bulk-isa-kit/` copy; Claude merges to main. Mission: kit `AGENTS.md`.
- Status: GREEN (MMX bank fix; MMX fuzz 12000/12000; shift/rotate flags 20000/20000;
  make test 458 passed / 0 failed). **Activate when Lane A/PE32 emit** an unsupported opcode /
  bad flags / wrong JIT helper / bad memory semantic / bad atomics.

### Lane C — Win32/NT loader + memory + Win32-DLL API breadth — Codex (2nd)  [STANDBY]
- `engine/wine/dlls/ntdll/unix/loader.c`, `virtual.c`
- `engine/wine/dlls/**` EXCEPT `ntdll/unix/macrunner_hb.c` and `ntdll/unix/signal_arm64.c`
  (Lane A) — i.e. kernel32, kernelbase, user32 native ARM64 impls.
- Mission: `reports/research/LANE-C-MISSION.md`. Status: clean (wineusb IDs, x64/EC breadth,
  HL slices, PE32 clipboard, GTA autorun/DX setup all clean).
- **Activate when triage emits** a missing import / loader / DLL / Win32 API failure.
- **HARD: never touch macrunner_hb.c / signal_arm64.c (Lane A), hb_* (Lane A/B), engine/dxmt (Lane D).**

### Lane D — Graphics / DXMT (D3D11→Metal) — Codex/Kimi  [STANDBY, blocked on A]
- `engine/dxmt/**`, `engine/vkd3d/**`, `engine/graphics/**`
- Mission: `reports/research/LANE-D-MISSION.md`. Status: D3D9 translate/Metal/headless PASS;
  D3D11 aarch64 headless PASS; **D3D11 x86_64 headless BLOCKED** (missing
  `engine/wine/dist/lib/wine/x86_64-unix/{winemac.so,ntdll.so}`).
- **Activate when** Lane A reaches `CreateDXGIFactory`/`D3D11CreateDevice`, OR x86_64-unix Wine
  artifacts are restored for an x86_64 D3D11 smoke.
- **HARD: never touch ntdll/ or hb_* (Lane A/B/C).**

### Lane E — Research + product/prep (no engine code) — Gemini (agy/Antigravity) / ChatGPT
- Produces briefs in `reports/research/CHATGPT-*.md` / `reports/research/GEMINI-*.md`. Edits no
  engine source. May edit product/analyzer Python (`app/configurator/**`) and Lane F/G prep docs.
- Current work: AI War 2 (GOG x64 Unity) compat research (installer type, DRM, DX, engine ver);
  Lane F/G oracle/smoke prep docs; triage generic-fallback design.
- **context-mode: YES** — agy has the full ctx toolset via `~/.gemini/config/mcp_config.json`
  (batch_execute/search/execute/fetch_and_index/insight/stats/…). Tell it in-terminal to use ctx.
- Account is separate (`ljyudith9@gmail.com`); its CONTEXT_MODE_DIR is default (not codex's
  `~/.codex/context-mode`) → for shared memory, run it in the same repo cwd or point it at the
  same dir. Interactive TUI (no clean headless exec) → operator-driven, not coordinator-spawned.

### Lane F/G — Legacy engines (Diablo 1 DDraw, GTA VC D3D8/RenderWare) + installers/repack — PREP ONLY
- Blocked on PE32 reaching `BTCpuSimulate`. Until then: only oracle/smoke docs, no runtime lane.
- F (legacy): Diablo1 PE32+DirectDraw/palette/blit; GTA VC PE32+D3D8+RenderWare+DInput/DSound.
- G (installers): needs PE32/WOW64 + CreateProcess + pipes + WaitForSingleObject + file I/O +
  large VirtualAlloc + syswow64 i386 DLL hygiene + unarc/FreeArc/lolz.

### Coordinator — Claude (this terminal, T0)
- All merges, conflict resolution, this matrix, status (`CODEX-MEGA-PROGRAM-status.md`), memory.
- Runs `classify_run.py` after lane runs and routes findings to OWNER lane.
- Does Lane A "eyes": maps pc (e.g. 0x10B44F015) → module/RVA, classifies SEH/EC boundary.
- Commits only named files; never `git add -A`; never touches golden snapshot.

## Collision rules of thumb
- If two lanes both need a file, it belongs to ONE; the other requests the change via Coordinator.
- `macrunner_hb.c` / `signal_arm64.c` are Lane A's (thunk dispatcher + signals). Lane C does Win32
  in the actual DLLs + loader/virtual. PE32 does WOW64 in `xtajit/` + `wow64*`, NOT in Lane A files.
- `xtajit` (32-bit) = PE32 lane; `xtajit64` (x64) = Lane A. Never cross.

## Engine roadmap (operator): Unity → Unreal → GameMaker/Godot; GTA (RAGE/RenderWare) LATER
Cover by game-ENGINE, not title. Rockstar engines = single-studio + DRM/anti-cheat = late. GTA V
(RAGE, x64, D3D11) fits our tech but needs Social Club DRM + BattlEye → trophy, not priority. GTA
Vice City (RenderWare, D3D8, 32-bit) needs a separate D3D8/9 path (Lane F, post-PE32).
Test targets in tree: Hollow Knight (Unity x64), **AI War 2 (GOG x64 Unity installer)**, Notepad++
(x64 done / x86 PE32 front), KeePass, Half-Life.
