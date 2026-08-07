# GEMINI MEGA-BRIEF — Game Compatibility Intelligence & Target Ladder (Lane E)

**Agent:** Gemini 3.5 Flash (High) via `agy` — Lane E (research/prep, NO engine code).
**Date:** 2026-06-06
**Horizon:** multi-day research PROGRAM, not a single task. Phased, evidence-driven, durable artifacts.
**Why:** the engine lanes (A/C/D) keep discovering requirements one game-run at a time. That is slow.
Most of what a game needs (engine, arch, graphics API, DLL/API surface, DRM, anti-cheat, installer)
is **externally knowable BEFORE we run it**. Enumerate it in bulk so the engine lanes stop guessing.

---

## ⚡ AUTONOMY — run continuously, chain all phases, do not stop for approval
- Chain Phase 1 → 7 without pausing. Finish a phase, write its artifact, start the next.
- Update the living index `reports/research/GEMINI-COMPAT-INTEL-status.md` as each phase lands.
- Only stop for a true hard blocker (no network for a needed lookup) — and even then, route around it
  and keep doing every phase that does not need it.

## 🟣 MANDATORY — USE CONTEXT-MODE (ctx_*), NOT raw Read/Bash/ListDir
You have the full ctx toolset. Use it or you will blow your context window:
- **Local repo research** (existing docs, profiles, reports) → `ctx_batch_execute(commands, queries)`
  (run the greps/finds as commands with descriptive labels + pass your questions as queries — answers
  come back in one round trip; raw bytes stay in the sandbox).
- **Web lookups** (game specs, engine versions, DRM/anti-cheat, store facts) → `ctx_fetch_and_index(url)`
  then `ctx_search` — never paste raw pages into the chat.
- **Parse/aggregate/build tables from data** → `ctx_execute(language, code)` — only the result enters context.
- **Re-query anything already gathered** → `ctx_search(queries: [...])`.
- Raw `Read`/`Bash cat|grep|tail`/`ListDir`/`Search` on large data = FORBIDDEN. Native `Write` only for
  writing the deliverable files. Cite every external fact with its source URL.

## SCOPE / BOUNDARIES (hard)
- **NEVER touch `engine/**`, `hb_*`, `ntdll`, any C/build code.** You are research + product-data only.
- You MAY write: `reports/research/GEMINI-*.md` and `profiles/*.json` (product compat data) and
  `docs/COMPAT-*.md`. Nothing else.
- Evidence not opinion: every claim = a source (URL, or a file:line in the repo, or an extracted
  binary header). Mark unknowns as `UNKNOWN — needs run` rather than guessing.

---

## PHASE 1 — Target Ladder (Tier 1/2/3), engine-first
Build `reports/research/GEMINI-target-ladder.md`: 30–50 Windows games as a prioritized queue, grouped
by ENGINE family per operator roadmap (Unity → Unreal → GameMaker/Godot → legacy RenderWare/RAGE).
Per game a row with: title · engine+version · arch (x86/x64) · graphics API (D3D9/10/11/12/Vulkan) ·
DRM · anti-cheat · store (GOG/Steam/Epic) · installer type · single-player? · est. difficulty (1–5) ·
why-good-or-bad-first-target. Apply the note-117 KILL-FILTER (kernel anti-cheat = EAC/BattlEye/Vanguard/
Ricochet/GameGuard/Denuvo-AC/PunkBuster → REJECT; DX12-only → later; launcher-hostile → avoid;
GOG DRM-free + Steam-direct-exe = clean). Cross-check the in-tree targets (Hollow Knight, AI War 2,
Notepad++/KeePass as proxies, Half-Life, GTA Vice City, Diablo 1). Output the Tier-1 top-10 first.

## PHASE 2 — Per-game deep tech profiles (Tier-1 top-10)
`reports/research/GEMINI-game-profiles.md` (+ machine-readable in Phase 7). For each Tier-1 game:
imported DLLs (static; extract from any local binary with ctx_execute on the PE header, else from
public data) · known dynamic LoadLibrary deps · DX feature level / shader model · .NET/Mono (+version) ·
audio stack (XAudio2/FMOD/Wwise/OpenAL) · input (XInput/DInput/RawInput) · networking · save/registry
path expectations · launcher/DRM wrapper · redistributables (VC++ runtime, .NET) the installer drops.

## PHASE 3 — Win32 / NT API & DLL surface map (proactive Lane C breadth)
`reports/research/GEMINI-api-surface-map.md`: the UNION of Win32/NT APIs + DLLs the Tier-1 games need
(from Phase 2 imports). Cross-reference against what MacRunner's Wine already provides (research the
repo's `engine/wine/dlls/**` presence with ctx_batch_execute — list of implemented DLLs, NOT reading
their source). Produce a GAP LIST: APIs/DLLs likely missing or stubbed that the Tier-1 set will hit →
hand to Lane C so it builds breadth BEFORE the games reach it. This is the single most useful output
for unblocking the engine proactively.

## PHASE 4 — Graphics requirements matrix (Lane D feed)
`reports/research/GEMINI-graphics-requirements.md`: per Tier-1 game → D3D version · required feature
level · shader model · swapchain/present mode · MSAA/compute/tessellation use · DXGI features ·
fullscreen/borderless behavior. Cross-ref against DXMT coverage (read `reports/research/DXMT-*.md`,
`DXMT-D3D11-COVERAGE.md` via ctx) → GAP LIST for Lane D: which D3D features the target set needs that
DXMT does not yet cover.

## PHASE 5 — Competitive matrix
`reports/research/GEMINI-competitive-matrix.md`: for each Tier-1 game, status on CrossOver, Whisky,
Apple GPTK, Mythic, Parallels, and (where known) native Mac port. Highlight where MacRunner's
pure-arm64 + HyperBridge (no Rosetta) path is the differentiator vs their x86_64-via-Rosetta stacks.
Cite sources (CodeWeavers compat DB, ProtonDB-equivalents, forums).

## PHASE 6 — Store / distribution & installer mechanics (Lane G prep)
`reports/research/GEMINI-store-distribution.md`: per store (GOG/Steam/Epic/Amazon/itch) the
download+auth+DRM mechanics and the GPL-firewall rule (note 119: store tools = separate process, never
linked; only butler MIT is linkable). DRM-free allowlist research. Installer/repack formats the engine
must survive (Inno Setup, NSIS, InstallShield, GOG, FreeArc/unarc/lolz) with the WOW64 stability
checklist (LAA 3GB, async pipes, process sync, syswow64 path hygiene). Reconcile with your earlier
`GEMINI-laneg-gtavc-d3d8-installer.md`.

## PHASE 7 — Analyzer profile-DB schema v3 + seed (product feed)
Propose `profiles/` JSON schema v3 for the AI configurator (`app/configurator/`): fields for engine,
arch, dx, anti-cheat, dlls, dynamic-deps, redistributables, recommended lane/env, known-issues.
Write the schema to `docs/COMPAT-profile-schema-v3.md` and SEED it: create `profiles/<game>.json` for
the Tier-1 top-10 from Phases 1–2. (This is product DATA, allowed. Do NOT modify analyzer Python logic
unless asked separately.)

---

## DELIVERABLES (all under reports/research/ unless noted)
1. `GEMINI-COMPAT-INTEL-status.md` (living index, update per phase)
2. `GEMINI-target-ladder.md`
3. `GEMINI-game-profiles.md`
4. `GEMINI-api-surface-map.md`  ← highest value for Lane C
5. `GEMINI-graphics-requirements.md`  ← Lane D
6. `GEMINI-competitive-matrix.md`
7. `GEMINI-store-distribution.md`
8. `docs/COMPAT-profile-schema-v3.md` + `profiles/<tier1>.json` seeds

## DEFINITION OF DONE
All 7 phases have their artifact, evidence-cited, with explicit GAP LISTS for Lane A (queue),
Lane C (API), Lane D (graphics). Unknowns marked `UNKNOWN — needs run`, not guessed. Coordinator
(Claude) will index these into the shared KB and route gap-lists to the owning lanes.
