# Kimi MEGA Master Brief — Full Graphics Stack Ownership

**Scope**: Take FULL ownership of MacRunner graphics stack from current scaffolding to production-validated Navisworks-ready system.

**Duration**: 3-6 months of focused work, multiple phases.

**Autonomy level**: Maximum. You own architectural decisions, implementation, testing, validation, documentation. Только TRUE blockers escalate.

**Conflict risk with Codex**: ZERO. Graphics is fully separate from HyperBridge/Wine ntdll work. No merge conflicts ever.

**Strategic importance**: This is the **North Star** for MacRunner. Navisworks (BIM viewer, Autodesk flagship), Revit, AutoCAD, Photoshop, modern games — все требуют functional graphics stack. Without it, MacRunner = productivity-text-apps only. With it, MacRunner = real-world Windows replacement on Mac.

---

## 0. Mandatory reading (in order)

1. [/Volumes/MacOS/MacRunner/AGENTS.md](../AGENTS.md) — **🎯 Zeroth Principle** first, then all mandatory protocols
2. [Strategic app ladder, Obsidian](file:///Users/timurtoby/Documents/MacRunner/92-strategic-app-ladder-roadmap.md) — context для why Navisworks
3. [Mythic competitive analysis, Obsidian](file:///Users/timurtoby/Documents/MacRunner/88-competitive-analysis-mythic.md) — why DXMT-direct better than DXVK→MoltenVK
4. Your previous audio + AOT briefs — same methodology pattern
5. This brief

---

## 1. Strategic context (read carefully)

### Why graphics is THE critical subsystem

MacRunner targets **productivity apps + eventually professional CAD/BIM**:

- 1С Тонкий клиент — OK without 3D
- KeePass — OK without 3D
- Notepad++ — OK without 3D
- AutoCAD LT — **NEEDS DirectX 9/11**
- Revit — **NEEDS DirectX 11 heavy**
- **Navisworks** — DirectX 11, massive 3D scenes (gigabyte BIM models)
- Photoshop — **NEEDS** DirectX 12 для GPU acceleration
- Modern games — DirectX 11/12, sometimes Vulkan

**Without graphics, MacRunner ceiling = text/numerical productivity.**
**With graphics, ceiling = full Windows ecosystem on Mac.**

### Competitive landscape — why we can win

Look at [88-competitive-analysis-mythic.md](file:///Users/timurtoby/Documents/MacRunner/88-competitive-analysis-mythic.md) for full analysis. TL;DR:

| Stack | Mythic / Whisky | CrossOver | MacRunner (our target) |
|---|---|---|---|
| DX 9/10/11 | DXVK → MoltenVK → Metal (3 layers) | DXVK → MoltenVK → Metal | **DXMT → Metal (1 layer direct)** + DXVK fallback |
| DX 12 | Apple D3DMetal (non-commercial license!) | Apple D3DMetal | **DXMT or VKD3D-Proton → Metal** |
| Vulkan | MoltenVK | MoltenVK | MoltenVK |
| Performance | -20-30% vs native | -15-25% vs native | **target -5-15% vs native** |
| Licensing | Cannot sell (GPTK) | Commercial | **LGPL commercial OK** |
| Rosetta dependency | Yes (dies 2027-2028) | Yes | **No (our HyperBridge)** |

**Our edge**: DXMT direct path = fewer translation layers = better performance + better licensing.

### Why DXMT is our default, not DXVK

DXMT (https://github.com/3Shain/dxmt):
- DirectX 11 → Metal directly via LLVM 15 IR (DXBC/DXIL → Metal IR translation)
- One layer translation vs three (DXVK → Vulkan IR → MoltenVK → Metal)
- Better performance, especially shader-heavy paths
- LGPL-compatible
- Active development

DXVK as **fallback**:
- Mature, battle-tested with thousands of Windows games
- DirectX 8/9/10 supported (DXMT might not initially)
- Use when DXMT doesn't handle particular app

VKD3D-Proton as **DirectX 12 path**:
- Proven by Steam Deck / Proton
- DirectX 12 → Vulkan → MoltenVK → Metal
- Until DXMT supports D3D12 directly

---

## 2. Current state — what's scaffolded

```
engine/dxmt/             — DXMT submodule/clone, status: scaffolded?
engine/dxvk/             — DXVK clone, status: scaffolded?
engine/dxvk-upstream/    — DXVK upstream tracking
engine/moltenvk/         — MoltenVK
engine/moltenvk-upstream/— MoltenVK upstream tracking
engine/vkd3d/            — VKD3D-Proton
engine/vkd3d-proton/     — possibly variant
engine/graphics/         — meta directory?
```

Wine d3d8 → d3d12 DLLs exist в `engine/wine/dlls/`:
- `d3d8`, `d3d9` — older APIs
- `d3d10`, `d3d10_1`, `d3d10core` — DX10 family
- `d3d11` — main DirectX 11
- `d3d12`, `d3d12core` — DirectX 12
- `d3dcompiler_33..47` — shader compilers (multiple versions)
- `dxgi` — DirectX graphics infrastructure
- `dxva2` — video acceleration

**Your job**: audit what's actually built, what works, what's hooked, then BUILD OUT the missing 80%.

---

## 3. Phased work plan (multi-month)

You own ALL phases. Sequential but each phase has deliverables you can complete and validate independently.

### Phase Γ.0 — Audit & strategy (1 week)

Like AOT cache work: audit first, plan second, code third.

Deliverables:
- `docs/GRAPHICS-AUDIT.md` — what exists, what builds, what works, what's hooked into Wine, what's missing
- `docs/GRAPHICS-STRATEGY.md` — architecture diagram, dispatch policy (DXMT vs DXVK vs VKD3D per scenario), licensing analysis confirming LGPL safe path
- `docs/GRAPHICS-ROADMAP.md` — your roadmap for Phases Γ.1 through Γ.6 с estimated timelines

Don't write code yet. Understand first.

### Phase Γ.1 — Build & integrate DXMT (2-3 weeks)

- Verify DXMT submodule builds на Apple Silicon
- Build pipeline: `scripts/build-dxmt.sh` produces installable libraries
- Wine `d3d11` DLL integration: replace/wrap Wine's d3d11 implementation with DXMT backend
- Initial smoke test: minimal D3D11 app (clear screen to color, present) renders correctly
- Document build process in `docs/GRAPHICS-BUILD.md`
- Tests: basic D3D11 device creation, swapchain, simple draw call

Success: a "hello triangle" D3D11 app renders correctly через DXMT → Metal на M1 Air.

### Phase Γ.2 — DXVK fallback path (2 weeks)

- Build DXVK in `engine/dxvk/` через MoltenVK
- Wine `d3d9`, `d3d10`, `d3d11` can switch to DXVK via env knob
- Dispatch policy: `MACRUNNER_GFX_BACKEND={dxmt,dxvk,auto}` 
- auto policy: try DXMT first, fall back to DXVK on init failure
- Test: same "hello triangle" works через DXVK path
- Test: DX9 app (older, DXMT might not support) works через DXVK

Success: both paths working, env knob toggles, auto fallback functional.

### Phase Γ.3 — DirectX 12 via VKD3D-Proton (2-3 weeks)

- Build VKD3D-Proton
- Wine d3d12 DLL hook to VKD3D-Proton
- Vulkan → MoltenVK → Metal pipeline для DX12
- Smoke test: D3D12 minimal app renders
- Performance baseline measurement

Success: DX12 apps боtаят, even if -30% vs native (acceptable for v1).

### Phase Γ.4 — Real-app smoke validation (3-4 weeks)

Test progression (each: install, run, verify rendering, measure perf, document):

1. **3DMark Time Spy** — synthetic benchmark, DX12, validates pipeline
2. **A simple D3D9 game** (Half-Life 2 demo, Portal, etc.) — DX9 via DXVK
3. **A simple D3D11 game** (Civilization V demo, etc.) — DX11 via DXMT
4. **AutoCAD LT trial** — DX11 productivity, hard real-world test
5. **SketchUp Free** — basic 3D modeling
6. **Revit student edition** — heavy DX11 BIM
7. **Navisworks Freedom** — North Star validation

Each app: report results в `docs/GRAPHICS-VALIDATION-{app-name}.md` with screenshots, perf numbers, known issues.

### Phase Γ.5 — Performance optimization (ongoing, 4+ weeks)

- Shader compilation caching (warm-start parity с AOT cache на CPU)
- Pipeline state caching
- Memory layout optimization для unified memory (Apple Silicon advantage)
- Multi-thread command list submission
- Hardware decode integration (VideoToolbox for video textures)

Target: 90% of native Mac performance for Metal-direct apps. 75-85% for translated DX paths.

### Phase Γ.6 — Polish & advanced features (4+ weeks)

- HDR support (DX11 + DXMT)
- Variable Refresh Rate (ProMotion на supported Macs)
- DLSS / FSR upscaler integration (DLSS not directly possible but FSR works)
- Multi-monitor support
- Display capture API (для apps which screenshot themselves)
- Stereo 3D / Spatial Audio integration (если apps use them)

### Phase Γ.7+ — Future (after Phase Γ.6)

Wave-tracing real-time stuff, Apple's M-series specific GPU features (sparse textures, raytracing acceleration на M3+), etc. Defer until basics solid.

---

## 4. Architectural decisions (your authority)

Per AGENTS.md Decision Rubric — you have autonomy on these. Examples of decisions you SHOULD make yourself (and document in design docs):

- **DXMT vs CrossOver's D3DMetal**: license check, performance check, decide. D3DMetal non-commercial precludes most cases for us.
- **VKD3D-Proton vs vkd3d (Wine upstream)**: Proton имеет more game compat. Use Proton fork unless specific reason against.
- **MoltenVK vs direct Vulkan-on-Metal**: MoltenVK only sane option.
- **Static link vs dynamic load of these libs**: dynamic (smaller binary, swappable per-app).
- **Per-app override system**: env vars + per-app config in `~/Library/Application Support/MacRunner/profiles/{app-name}.toml`. Profile per-app GFX choice.
- **Driver shim packaging**: build as Wine driver DLLs (.dll.so equivalent), drop into `dist-pure-arm64/lib/wine/aarch64-windows/`.

Decisions you SHOULD escalate (write Questions for Timur):
- Major licensing risk found (non-LGPL component in critical path)
- Need to change Wine's core IDirect3D* COM interfaces (Codex's territory; coordinate)
- Strategic pivot (e.g., decide D3DMetal worth licensing fee from Apple)

---

## 5. Workspace

### Where you write

```
/Volumes/MacOS/MacRunner/
├── engine/dxmt/                              # DXMT submodule/clone — YOUR PRIMARY
├── engine/dxvk/                              # DXVK — YOUR PRIMARY
├── engine/vkd3d/, vkd3d-proton/              # VKD3D — YOUR PRIMARY
├── engine/moltenvk/                          # MoltenVK — YOUR PRIMARY
├── engine/graphics/                          # YOUR meta-directory для dispatch logic, shim glue
│   ├── dispatch/                              # gfx backend dispatcher
│   ├── shaders/                               # shader cache, debug shaders
│   └── profiles/                              # per-app profile loader
├── engine/wine/dlls/d3d*/                    # CAREFUL: modify build to link DXMT/DXVK
├── engine/wine/dlls/dxgi/                    # CAREFUL: modify build for DXMT swapchain
├── scripts/                                   # ADD build scripts
│   ├── build-dxmt.sh
│   ├── build-dxvk.sh
│   ├── build-vkd3d.sh
│   ├── build-graphics-all.sh
│   └── run-graphics-test-*.sh
├── docs/                                      # MASSIVE documentation
│   ├── GRAPHICS-AUDIT.md
│   ├── GRAPHICS-STRATEGY.md
│   ├── GRAPHICS-ROADMAP.md
│   ├── GRAPHICS-BUILD.md
│   ├── GRAPHICS-DISPATCH-POLICY.md
│   ├── GRAPHICS-VALIDATION-{app}.md          # per real-app validation
│   ├── GRAPHICS-PERFORMANCE.md
│   ├── GRAPHICS-KNOWN-ISSUES.md
│   └── KIMI-PROGRESS-graphics.md             # YOUR PROGRESS LOG
└── artifacts/graphics-test/                  # test apps, screenshots, traces
```

### Do NOT touch (Codex's territory or other agent territory)

- `engine/hyperbridge/` — Codex active work, AND Kimi AOT cache work
- `engine/wine/dlls/ntdll/` — Codex
- `engine/wine/dlls/kernelbase/` — Codex
- `engine/wine/dlls/win32u/` — Codex (window metrics, USER32 internals)
- `engine/audio/` — Kimi audio stack work (your previous brief)
- `engine/wine/dlls/winecoreaudio.drv/` — your previous audio brief

Wine d3d* DLLs are SHARED — they belong technically to Wine itself, but you'll be modifying their build to integrate DXMT/DXVK/VKD3D backends. If you change Wine d3d source code itself (rare), coordinate via Questions for Timur.

---

## 6. Methodology (zero copy from audio/AOT briefs — same rules)

### Zeroth principle applies everywhere
- Native Metal API direct когда possible (Phase Γ.6+ может иметь Metal-direct path для some apps)
- Root-cause every found issue — no "TODO fix later" without explicit subtask
- No workarounds, only proper fixes

### Family audit triggered by
- Found one D3D11 API gap → audit все D3D11 APIs of that family
- Found one HLSL→MSL shader translation issue → audit все shader idioms in that class
- Found one render target binding bug → audit все RT scenarios

### Patch-by-evidence
- Profile before optimizing
- Measure before claiming "fixed"
- Real GPU traces (Metal System Trace) before architectural changes

### Process hygiene (same as Wine work)
- Clean up rendering processes between runs
- Wine prefix per test (isolated)
- Codesign DLLs after build

### Decision autonomy
- Don't ping for typical engineering choices
- Document decisions in design docs
- Only escalate TRUE blockers (licensing, strategic pivots, cross-Codex impacts)

---

## 7. Communication protocol

### Progress log: `docs/KIMI-PROGRESS-graphics.md`

Per session:

```markdown
## YYYY-MM-DD — Phase Γ.X, Session N

What was done:
- [bullet points]

Decisions made:
- [bullet с rationale, link к design doc]

Tests/validation:
- [what verified, what numbers]

Blockers / Questions for Timur:
- [TRUE blockers only]

Next session plan:
- [bullet points]
```

### Per-phase closeout

At end of each Phase Γ.X, write summary в `docs/GRAPHICS-PHASE-Γ-X-CLOSURE.md`:
- All deliverables shipped
- Performance numbers
- Known issues with severity
- Recommendation: proceed to next phase OR address issue first

### Big decisions: write architectural decision record (ADR)

Format: `docs/GRAPHICS-ADR-NNN-{title}.md`

```markdown
# ADR-NNN: {Title}

Status: Accepted / Superseded / Proposed
Date: YYYY-MM-DD

## Context
[problem space]

## Decision
[what was decided]

## Rationale
[why this over alternatives]

## Consequences
[good and bad outcomes]

## Alternatives considered
[what we didn't pick and why]
```

ADRs survive context compaction and onboard future agents instantly. Write them for any non-trivial decision.

---

## 8. Success criteria per phase (you confirm via tests + measurements)

### Phase Γ.1 success
- [ ] `scripts/build-dxmt.sh` builds clean from fresh checkout
- [ ] DXMT loaded by Wine d3d11.dll at runtime
- [ ] "Hello triangle" D3D11 app renders correctly (visual verification)
- [ ] No memory leaks (5min run, monitor RSS)
- [ ] No crashes on init/teardown

### Phase Γ.2 success
- [ ] DXVK builds and loads
- [ ] DX9 app renders через DXVK
- [ ] Env knob switches between DXMT and DXVK
- [ ] Auto-fallback works (DXMT fails → DXVK takes over)

### Phase Γ.3 success
- [ ] VKD3D-Proton builds
- [ ] D3D12 minimal app renders
- [ ] No crashes on basic D3D12 init

### Phase Γ.4 success
- [ ] 3DMark Time Spy completes with reasonable score
- [ ] At least one real game runs playably
- [ ] AutoCAD LT trial opens, can create simple drawing
- [ ] SketchUp Free works for basic models
- [ ] Revit student edition opens (even if slow)
- [ ] **Navisworks Freedom loads a sample BIM model** — North Star check

### Phase Γ.5 success
- [ ] Shader cache hit rate > 90% after warm-up
- [ ] Frame time improvement measured vs Phase Γ.4 baseline
- [ ] Real app performance: >70% of native Mac equivalent

### Phase Γ.6 success
- [ ] HDR test app shows HDR output на supported display
- [ ] Multi-monitor test app spans correctly
- [ ] FSR upscaler integrates без crashes

---

## 9. Reference materials

### DXMT
- Project: https://github.com/3Shain/dxmt
- Architecture: LLVM 15 IR translation, see DXMT design docs
- Apple's relevant docs: Metal Shading Language, Metal Performance Shaders

### DXVK
- Project: https://github.com/doitsujin/dxvk
- Mature codebase, well-documented
- Wine integration patterns

### VKD3D-Proton
- Project: https://github.com/HansKristian-Work/vkd3d-proton
- Proton-flavored Wine D3D12 → Vulkan
- Reference: how Steam Deck handles D3D12 apps

### MoltenVK
- Project: https://github.com/KhronosGroup/MoltenVK
- Vulkan → Metal layer
- Apple-recognized standard

### Wine internals
- `engine/wine/dlls/d3d11/` — Wine's D3D11 implementation
- `engine/wine/dlls/dxgi/` — swapchain, factory
- `engine/wine/dlls/d3dcompiler_*/` — shader compiler entries

### Apple
- Metal: https://developer.apple.com/metal/
- Metal Performance Shaders
- Metal Compute
- Metal-cpp (Objective-C++ wrapper)

### Testing tools
- Metal System Trace (Xcode Instruments) — GPU profiling
- Metal Validator (env var `MTL_DEBUG_LAYER=1`)
- RenderDoc (Vulkan capture)
- ApiTrace (D3D capture)

---

## 10. First session — what to do exactly

Don't write graphics code в session 1. Audit и design.

1. Read AGENTS.md fully (especially Zeroth Principle)
2. Read this brief fully
3. Inspect actual contents:
   ```bash
   ls -la engine/dxmt/ engine/dxvk/ engine/vkd3d/ engine/moltenvk/
   ls -la engine/wine/dlls/d3d11/ engine/wine/dlls/dxgi/
   ```
4. Determine what's scaffolded vs empty
5. Read DXMT README / docs if cloned, get sense of project state
6. Create `docs/GRAPHICS-AUDIT.md` with skeleton:
   - What exists в repo
   - What builds
   - What's hooked
   - What's missing
7. Create `docs/KIMI-PROGRESS-graphics.md` first entry
8. Plan Session 2 (start audit detail, decide first build target)

Session 2-5: complete Phase Γ.0 audit and strategy. Don't move to Phase Γ.1 builds until strategy doc reviewed.

Session 6+: Begin Phase Γ.1.

---

## 11. Timeline expectations (rough — adjust as you learn)

| Phase | Calendar weeks | Sessions est. |
|---|---|---|
| Γ.0 Audit & strategy | 1 | 5-7 |
| Γ.1 DXMT build & integrate | 2-3 | 12-15 |
| Γ.2 DXVK fallback | 2 | 8-10 |
| Γ.3 D3D12 via VKD3D | 2-3 | 10-12 |
| Γ.4 Real-app validation | 3-4 | 15-20 |
| Γ.5 Performance optimization | 4+ | 20+ |
| Γ.6 Polish & advanced | 4+ | 20+ |
| **Total to v1 production** | **18-22 weeks** | **90-110 sessions** |

This is a serious multi-month investment. You're the primary engineer on graphics. Treat it as full ownership.

---

## 12. Coordination touchpoints

### With Codex (when applicable)

- If you need Wine `IDirect3D*` interface changes — Codex coordinates
- If you find HyperBridge bugs affecting graphics (e.g. SSE/AVX issues in shader code path) — file detailed report, Codex fixes
- If guest x64 app's graphics calls don't dispatch correctly — boundary issue, Codex helps

But 95% of your work is independent. Coordination rare.

### With Timur

- Phase closeouts (review summary, approve next phase)
- ADRs for architectural decisions
- Real-app validation results (specifically Navisworks — this is the milestone)
- Licensing concerns

### With other Kimi work streams

You also own audio stack (previous brief, mostly done) and AOT cache (brief #2, in progress). Allocate time:
- Audio: maintenance, E2E test когда Wine stable
- AOT cache: continue Phase 1+2, integrate Phase 3 после Codex Phase H closure
- **Graphics: primary focus going forward** — this is months of work, give it majority time

---

## 13. The vision

When you're done with all phases:

- MacRunner runs **DirectX 9, 10, 11, 12** apps
- Performance within **80-95% of native Mac**
- Navisworks loads gigabyte BIM models smoothly
- AutoCAD, Revit usable for real production work
- Modern games playable
- HDR, VRR, multi-monitor supported
- Shader compilation cached, warm starts fast
- **This is the moat** that makes MacRunner buy-able for $50/seat from professional Mac users

Mythic / Whisky can't match this. CrossOver can but at $74/year subscription. MacRunner at premium price point with **no Rosetta dependency** (their 2027 deathtimer doesn't affect us) — viable commercial product.

---

## Appendix A — Path quick reference

```
Repo root:              /Volumes/MacOS/MacRunner
Your primary workspace: engine/dxmt/, engine/dxvk/, engine/vkd3d/, engine/graphics/
Wine d3d DLLs:          engine/wine/dlls/d3d8/ through d3d12/, dxgi/
Documentation:          docs/GRAPHICS-*.md
Progress log:           docs/KIMI-PROGRESS-graphics.md
Test apps:              artifacts/graphics-test/{app-name}/
```

## Appendix B — Sanity check first

```bash
cd /Volumes/MacOS/MacRunner
. config/env.sh
echo "Root: $MACRUNNER_ROOT"
ls -la engine/dxmt/ 2>/dev/null | head -10
ls -la engine/dxvk/ 2>/dev/null | head -10
ls -la engine/vkd3d/ 2>/dev/null | head -10
ls -la engine/graphics/ 2>/dev/null | head -10
ls engine/wine/dlls/ | grep -E '^d3d|^dxgi' | sort
```

Reports what's там. Audit starts from this.

---

End of brief. **You own graphics stack from now until production v1.** Apply Zeroth Principle relentlessly. 

Make MacRunner ready for Navisworks.
