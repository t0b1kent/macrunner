# Kimi MEGA Master Brief — AI Auto-Configurator (THE Moat)

**Scope**: Build AI-driven automatic configuration system для MacRunner — the **strategic moat** which Mythic/Whisky/CrossOver lack. Analyze any Windows PE → predict optimal bottle config → auto-tune via error log feedback → share community-validated profiles.

**Duration**: 5-8 months focused work, 8 phases.

**Autonomy level**: Maximum. Full architectural ownership, ML model choices, data schema, cloud integration, validation strategy.

**Conflict risk**: ZERO with Codex (engine territory). LOW с existing configurator (you extend, не rewrite). MEDIUM coordination с networking brief (Phase G uses cloud API from networking brief).

**Strategic value**: This is **the commercial differentiator**. Wine alone — open source, free competitors. AI auto-config + community profiles — paid product feature что **nobody else has**. When Mythic dies в 2027 (Rosetta deprecation), MacRunner survives с both: (1) own translator, (2) AI auto-config moat.

---

## 0. Mandatory reading

1. [/Volumes/MacOS/MacRunner/AGENTS.md](../AGENTS.md) — **🎯 Zeroth Principle**, protocols
2. [/Users/timurtoby/Documents/MacRunner/88-competitive-analysis-mythic.md](file:///Users/timurtoby/Documents/MacRunner/88-competitive-analysis-mythic.md) — **why AI is the moat**, в деталях
3. [/Users/timurtoby/Documents/MacRunner/92-strategic-app-ladder-roadmap.md](file:///Users/timurtoby/Documents/MacRunner/92-strategic-app-ladder-roadmap.md) — app ladder context
4. Your other briefs (especially networking — Phase G cloud) — coordination points
5. This brief

---

## 1. Strategic context — why AI is THE moat

### Competitive landscape

```
                  | Wine | Mythic | Whisky | CrossOver | MacRunner
Translator        | ✓    | (Rosetta) | (Rosetta) | (Rosetta) | OWN (HB)
Graphics          | -    | DXVK/D3DMetal | DXVK | D3DMetal | DXMT+DXVK
LGPL commercial   | -    | ✗ (GPTK)  | ✗ (GPTK) | ✓         | ✓
**AI auto-config**| -    | ✗        | ✗      | ✗        | **✓** (this brief)
**Community profiles** | - | ✗      | ✗      | (paid DB) | **✓** (cloud)
Rosetta dependency| n/a  | YES (2027 dies) | YES | YES    | NO
```

The **AI auto-configurator** = **defensible moat**. Anyone can fork Wine. Anyone can integrate DXVK. AI requires:
- Months of data collection
- ML model trained on community feedback
- PE binary analysis library
- Error log parsing intelligence
- Cloud distribution infrastructure

It takes **time** to replicate. By the time competitors catch up, MacRunner has больше users → больше data → better model → стronger moat.

### The user value loop

```
User installs Windows app
  ↓
MacRunner auto-detects PE → analyzes binary signatures, imports, dependencies
  ↓
Cloud lookup: hash matches community-validated profile?
  ↓ yes              ↓ no
Apply profile        AI model predicts optimal profile from features
  ↓                  ↓
Launch app           Apply prediction
  ↓                  ↓
Success?    User feedback (works / crashes / specific issue)
  ↓ no
Error log parser → suggests fix → user accepts → re-launch
  ↓ success after N attempts
Profile uploaded к cloud (anonymized) → community benefit
  ↓
Other users with same PE hash get this profile instantly
```

**Result**: zero manual configuration. Click app, it just runs.

Compare to Mythic/Whisky/CrossOver:
- Open container settings
- Pick DX backend (DXMT vs DXVK vs D3DMetal)
- Configure registry overrides
- Tune env vars
- Try, fail, retry, fail again
- Eventually give up OR succeed by chance

**MacRunner experience**: drag .exe → just works (если profile есть в community OR AI predicts correctly).

---

## 2. Current state — what's scaffolded

You're not starting from scratch. There's substantial existing work:

```
app/configurator/
├── compatibility.py        (1243 lines) — main compat logic
├── exe_analyzer.py         (178 lines)  — basic PE analysis
├── profiles.py             (185 lines)  — profile management
├── bottles.py              (183 lines)  — bottle creation
├── engines.py              (149 lines)  — engine selection
├── inventory.py            (269 lines)  — app inventory
├── anticheat.py            (44 lines)   — anti-cheat detection
├── pe.py                   (78 lines)   — PE primitives
├── smoke.py                (129 lines)  — smoke testing
└── ...

profiles/                    (existing community profiles)
├── 1c-enterprise-83.json
├── business-autocad-2025.json
├── business-office-generic.json
├── game-anticheat-safe.json
├── game-cyberpunk-2077.json
├── game-generic-dx11.json
├── game-generic-dx12.json
├── game-generic-dx9.json
├── game-mortal-kombat-1.json
├── control-center-manual-apps.json

tools/compat_db.py          (43 lines)   — compat DB primitives
tools/compat-runner/        (extensive)
```

**What exists**: PE inspection, profile JSON format, runner for testing apps, anti-cheat detection, basic inventory.

**What's missing (your work)**:
1. Deep PE binary analysis (imports, dependencies, anti-tamper signatures)
2. Profile inheritance system (template + deltas, не каждый app — own copy)
3. Error log parser (Wine stderr → actionable fix suggestions)
4. AI/ML model для прогнозирования profile для unknown apps
5. Auto-tuning loop (try config → measure → adjust → retry)
6. Community sharing protocol (cloud upload/download, validation, anti-spam)
7. Per-user privacy-respecting feedback collection
8. Cloud server integration (uses networking brief Phase Δ.5 backend)
9. Conflict resolution (multiple profiles for same hash — pick best by user feedback)
10. End-to-end validation с 20+ real apps

---

## 3. Phased work plan

### Phase A.0 — Audit + design (1-2 weeks, 5-10 sessions)

Like other mega-briefs: audit first, design second, code third.

Deliverables:
- `docs/AI-CONFIGURATOR-AUDIT.md` — what configurator does today vs gaps
- `docs/AI-CONFIGURATOR-STRATEGY.md` — overall architecture, ML approach, data flow
- `docs/AI-CONFIGURATOR-ROADMAP.md` — phases A.1-A.7 timeline
- `docs/AI-CONFIGURATOR-DATA-SCHEMA.md` — profile JSON v2 spec, telemetry schema, ML feature vector
- First 4 ADRs:
  - **ADR-A001**: PE analysis library (libpe vs lief vs custom)
  - **ADR-A002**: ML model architecture (rule-based + decision tree + neural? gradient boosting? simple kNN?)
  - **ADR-A003**: Error log parsing approach (regex + LLM hybrid? pure regex? full LLM?)
  - **ADR-A004**: Community profile validation (manual moderation vs automated trust score vs hybrid)

### Phase A.1 — Deep PE binary analyzer (3-4 weeks)

Replace existing simple `exe_analyzer.py` (178 lines) с production-grade analyzer.

**Features**:
- Parse full PE32+ headers
- Extract: imports, exports, sections, resources, signatures
- Detect: anti-cheat (BattlEye/EAC/Vanguard/PunkBuster), DRM (Denuvo, VMProtect, Themida)
- Identify: framework (Qt, WPF, GTK, win32, Electron-on-Windows)
- Architecture: x86, x64, ARM64EC, AnyCPU
- Bitness, CRT version, .NET version (if .NET app)
- Resource analysis: icons, manifests, version strings
- Heuristics: is this a "launcher" vs "actual app"?

**Output**: structured features vector (~50-100 dimensions) для ML model input.

**Tests**: validate against existing 10+ profile apps, accuracy >95% feature detection.

### Phase A.2 — Profile inheritance + template system (2-3 weeks)

Current profiles — flat JSON files per app. **Need**: hierarchical inheritance.

**Design**:
```
templates/
├── _base.json                    — defaults
├── _category-business.json       — inherits _base
├── _category-game.json           — inherits _base
├── _category-game-dx9.json       — inherits _category-game
├── _category-game-anticheat.json — inherits _category-game

profiles/
├── autocad-2025.json             — inherits _category-business + own deltas
├── cyberpunk-2077.json           — inherits _category-game-dx12 + own deltas
```

Override resolution: deeper child wins. Conflict logging. Versioning per template.

**API**:
```python
profile = ProfileEngine.resolve(app_hash="abc...", template_chain=["_category-game", "_category-game-dx12"])
profile.dx_backend  # = "dxmt" (from template default, not overridden)
profile.env["DXVK_HUD"]  # = "fps" (from this app's override)
```

### Phase A.3 — Error log parser + suggestion engine (3-4 weeks)

Wine stderr is **goldmine** для debugging. Today users see "err:module:ldr_module_get_proc_address (00007F...) failed" — meaningless to humans.

**Parser**:
- Tokenize Wine debug output (err:/warn:/fixme: prefixes)
- Extract: error code (NTSTATUS, WSAERROR, HRESULT), module, function, parameters
- Pattern match against known issue database
- Output: human-readable description + suggested fix

**Suggestion engine**:
- Known patterns: "missing DLL X → install via winetricks Y", "WinSock TLS handshake failed → enable MACRUNNER_NET_BACKEND=nw"
- Apply suggestion automatically (с user confirmation) → relaunch app

**LLM integration (optional)**:
- For unknown error patterns, send anonymized error to cloud → LLM analyzes → returns suggestion
- Cache result для same error pattern
- This is **expensive** so off by default — opt-in для users wanting help

**Tests**: 50+ known error patterns covered, parser precision >95%, suggestion accuracy >80%.

### Phase A.4 — Auto-tuning loop (2-3 weeks)

When AI predicts profile, it might be wrong. Auto-tuning iterates:

```
1. Apply predicted profile
2. Launch app, monitor stderr/process state
3. If error log matches known pattern → apply suggestion, re-launch
4. If process exits cleanly within N seconds → assume success, save profile
5. If process crashes → analyze, suggest, retry up to K times
6. If still crashes after K → fallback to manual mode + report
```

**Critical**: budget enforcement (don't auto-retry forever), preservation of user's data (don't wipe bottle on retry), rollback ability.

**API**:
```python
result = AutoTuner.run(app="path/to/app.exe", max_attempts=5)
# result.success: bool
# result.profile: ResolvedProfile
# result.attempts: List[AttemptLog]
# result.suggestion: Optional[str]  # if failed, what to try manually
```

### Phase A.5 — ML model for prediction (4-6 weeks)

For unknown PE hashes (not in community profile DB), AI predicts optimal profile.

**Features (input)**:
- PE binary feature vector (Phase A.1)
- App name patterns
- Installer metadata (from NSIS/MSI/InnoSetup)
- File path heuristics
- User's macOS version, RAM, CPU

**Output (prediction)**:
- DX backend (DXMT vs DXVK vs none)
- Audio backend (avaudio vs wine-native)
- Engine variant (HyperBridge vs Rosetta for installer)
- Required winetricks packages
- Env vars (registry overrides, DXVK_HUD, MACRUNNER_HB_*)

**Model approach** (per ADR-A002):
- **MVP**: rule-based + decision tree on 50-100 training apps
- **V2**: gradient boosting (XGBoost/LightGBM) on 500+ apps from community
- **V3 (stretch)**: small neural net (transformer over PE features) if data volume justifies

**Training**:
- Initial: hand-labeled 50 apps (you already have 10 profiles, add 40 more)
- Continuous: community profile uploads (anonymized) feed training set
- Validation: held-out set, predict → compare к actual community profile, accuracy metric

**Inference**:
- Local: rule-based + decision tree (no network needed)
- Cloud: gradient boosting + larger model (online if available)

### Phase A.6 — Community sharing + cloud integration (3-4 weeks)

Profile sharing requires cloud backend (from networking brief Phase Δ.5).

**Endpoints used**:
- `POST /profiles/contribute` — user uploads profile after successful tuning
- `GET /profiles/{app-hash}` — fetch profile для new install
- `GET /profiles/popular` — top 100 (cache locally)
- `POST /feedback` — works/broken signal с anonymized telemetry

**Validation pipeline** (server-side):
- Anti-spam: rate limit per user
- Trust score: profile from user who's contributed N working profiles gets higher trust
- Manual moderation queue для high-stakes profiles (top 50 popular apps)
- Automated checks: profile JSON validates against schema, no executable code, sane bounds on values

**Conflict resolution**:
- Multiple profiles for same hash → ranked by:
  - Trust score of contributor
  - Recent successful runs (last 30 days)
  - Specific user's macOS version match
- Pick highest, fallback to next if user reports issue

**Privacy**:
- Profile contributions strictly opt-in
- All anonymized (no PII)
- User can withdraw their profiles
- GDPR deletion endpoint

### Phase A.7 — Production validation (4+ weeks)

End-to-end testing с 20+ real apps progression:

1. **Tier 1 productivity** (5 apps): Notepad++, 7-Zip, VLC, OBS, foobar2000
2. **Tier 2 business** (5 apps): Office viewer, AutoCAD LT trial, SketchUp Free, FileZilla, PuTTY
3. **Tier 3 specialty** (5 apps): 1С Тонкий клиент, KeePass 1.x/2.x, Revit student
4. **Tier 4 games** (5 apps): variety of DX versions, anti-cheat off, simple games
5. **Tier 5 edge cases** (5 apps): .NET heavy, weird installers, anti-tamper, obscure frameworks

For each:
- Drop binary, run AutoConfigurator
- Measure: time to first successful launch, attempts needed, profile accuracy
- Document: `docs/AI-CONFIGURATOR-VALIDATION-{tier}.md`
- Iterate model на failures

**Success criteria**:
- >80% Tier 1-3 apps auto-launch within 1 attempt
- >50% Tier 4-5 within 3 attempts
- 0% data loss across all tests
- Avg latency < 30s from drop к first launch

---

## 4. Architectural decisions (your authority)

Per AGENTS.md Decision Rubric:

- **PE analysis library**: libpe (C, fast), lief (Python, friendly), custom (most control)? **Recommend lief** for MVP.
- **ML framework**: scikit-learn (simple, mature), XGBoost (powerful), PyTorch (overkill для V1)? **Recommend XGBoost** для V2.
- **Profile schema versioning**: how migrate when schema changes? Built-in migration scripts.
- **Anti-cheat detection**: refuse to launch (legal safe) vs warn user vs hide MacRunner identity (legally risky)? **Recommend warn + refuse to inject specific anti-cheats**.
- **LLM cost**: opt-in только? Pre-cache popular errors? **Recommend opt-in с aggressive caching**.
- **Cloud sync frequency**: real-time vs batch daily? **Recommend batch daily + on-demand when error encountered**.

Escalate (Timur):
- Commercial: pricing tier (free profiles vs premium AI?)
- Legal: anti-cheat policy (some are illegal to circumvent в certain jurisdictions)
- Brand: AI feature naming (Smart Config vs Auto Config vs ...)
- Partnership: integration с launcher manufacturers (Steam, Epic, Battle.net APIs)

---

## 5. Workspace

```
/Volumes/MacOS/MacRunner/
├── app/configurator/                              # EXTEND existing (with care)
│   ├── ai/                                         # NEW your AI subsystem
│   │   ├── pe_analyzer/                            # Phase A.1 — deep PE inspection
│   │   ├── profile_engine/                         # Phase A.2 — inheritance + templates
│   │   ├── error_parser/                           # Phase A.3 — log parsing
│   │   ├── auto_tuner/                             # Phase A.4 — feedback loop
│   │   ├── ml_model/                               # Phase A.5 — prediction
│   │   ├── community/                              # Phase A.6 — cloud sharing
│   │   └── validation/                             # Phase A.7 — E2E tests
│   └── (existing files — extend, don't rewrite)
├── profiles/                                       # EXTEND existing template system
│   ├── templates/                                  # NEW hierarchical templates
│   │   ├── _base.json
│   │   ├── _category-*.json
│   ├── (existing flat profiles migrate to inherit templates)
├── docs/
│   ├── AI-CONFIGURATOR-AUDIT.md
│   ├── AI-CONFIGURATOR-STRATEGY.md
│   ├── AI-CONFIGURATOR-ROADMAP.md
│   ├── AI-CONFIGURATOR-DATA-SCHEMA.md
│   ├── AI-CONFIGURATOR-ADR-NNN-*.md               # multiple ADRs
│   ├── AI-CONFIGURATOR-VALIDATION-*.md            # per-tier validation results
│   └── KIMI-PROGRESS-ai-configurator.md           # YOUR PROGRESS LOG
└── tests/
    └── ai-configurator/                            # extensive unit + integration
```

### Do NOT touch
- `engine/hyperbridge/`, `engine/wine/dlls/**` — Codex
- `engine/audio/`, `engine/networking/`, `engine/hyperbridge/cache/` — your other streams
- `engine/graphics/` — graphics done

### Coordination
- Phase A.6 uses cloud API from your networking brief Phase Δ.5 — must align endpoint schemas
- Profile schema relates к engine config (HyperBridge env vars) — must stay в sync с Codex changes

---

## 6. Methodology (same as other briefs)

### Zeroth Principle
- Native macOS metadata sources где applicable (Spotlight, LaunchServices)
- Root-cause every misclassification — улучшай ML, не workaround
- No silent failures (Phase A.4 must log every attempt, не пропускать failures)

### Family audit
- Found one PE signature missing → audit все sig patterns
- Found one error pattern not parsed → audit related error families

### Patch-by-evidence
- ML accuracy claims require validation set numbers
- "Works for me" insufficient — test set required

### Decision autonomy
- Most ML / data / API decisions — yours, document via ADRs
- Escalate только: commercial, legal, brand, partnerships

---

## 7. Communication

`docs/KIMI-PROGRESS-ai-configurator.md` per session:

```markdown
## YYYY-MM-DD — Phase A.X, Session N

What was done:
- [bullets]

Decisions (per autonomy):
- [ADR refs]

Validation results:
- [metrics if applicable]

Questions for Timur:
- [TRUE blockers only]

Next session plan:
- [bullets]
```

Per-phase closeout: `docs/AI-CONFIGURATOR-PHASE-A-X-CLOSURE.md`

ADRs prolifically — AI/ML decisions are many.

---

## 8. Success criteria (per phase)

### Phase A.1 (PE analyzer)
- >95% feature extraction accuracy on 100 test PE files
- 50+ anti-cheat/DRM signatures detected
- < 100ms analysis per binary

### Phase A.2 (Profile engine)
- All existing profiles migrate to template system
- Template resolution < 10ms
- Schema versioning + migration tested

### Phase A.3 (Error parser)
- 50+ Wine error patterns recognized
- >80% suggestion accuracy on held-out set
- LLM fallback opt-in, cached aggressively

### Phase A.4 (Auto-tuner)
- Successfully tunes 80% of test apps within 3 attempts
- 0% data loss across 100 iterations
- Budget enforcement (no infinite retry)

### Phase A.5 (ML model)
- MVP rule-based: >70% accuracy on 50 known apps
- V2 XGBoost: >85% accuracy
- Training pipeline reproducible

### Phase A.6 (Community sharing)
- Upload/download integration working
- Anti-spam survives stress test (100 fake uploads/min)
- Privacy audit passed

### Phase A.7 (Validation)
- 20+ apps tested, results documented
- Tier 1-3: >80% auto-launch first attempt
- Tier 4-5: >50% within 3 attempts

---

## 9. Reference materials

### PE analysis
- libpe: https://github.com/merces/libpe
- lief: https://lief.re (recommended для Python integration)
- pefile (Python): https://github.com/erocarrera/pefile

### ML
- scikit-learn: classic, simple
- XGBoost: production-grade gradient boosting (recommended V2)
- LightGBM: alternative to XGBoost, sometimes better для small data
- PyTorch (if neural net later): only if data volume justifies

### Wine error patterns
- Wine debug channels: https://wiki.winehq.org/Debug_Channels
- Common winetricks fixes: https://github.com/Winetricks/winetricks
- Wine AppDB (large community profile DB, не GDPR-compliant): https://appdb.winehq.org

### Competitor references
- CrossOver compatibility DB (paid, but visible categories)
- Lutris install scripts (community-driven, Linux focus, но lots of overlap)
- ProtonDB (Steam Deck community profiles)

### Anti-cheat / DRM
- BattlEye: https://www.battleye.com (commercial, has detection signatures)
- Denuvo: published bypass techniques (DO NOT IMPLEMENT — legal risk)
- VMProtect: detection возможна, just identify, don't bypass

---

## 10. First-session goals

Session 1 — read + plan, no code:

1. Read AGENTS.md (especially Zeroth Principle)
2. Read this brief fully
3. Inspect existing configurator:
   ```bash
   wc -l app/configurator/*.py
   ls profiles/
   cat profiles/1c-enterprise-83.json
   cat profiles/business-autocad-2025.json
   cat profiles/game-cyberpunk-2077.json
   ```
4. Read existing compatibility.py (1243 lines) carefully — understand current logic
5. Create skeletons:
   - `docs/AI-CONFIGURATOR-AUDIT.md`
   - `docs/KIMI-PROGRESS-ai-configurator.md`
6. Plan Session 2: complete audit, start Strategy doc

Session 2-4: complete Phase A.0 (audit + strategy + roadmap + first 4 ADRs).
Session 5+: begin Phase A.1 (PE analyzer).

---

## 11. Timeline expectations

| Phase | Weeks | Sessions |
|---|---|---|
| A.0 Audit & strategy | 1-2 | 5-10 |
| A.1 PE analyzer | 3-4 | 15-20 |
| A.2 Profile engine | 2-3 | 10-15 |
| A.3 Error parser | 3-4 | 15-20 |
| A.4 Auto-tuner | 2-3 | 10-15 |
| A.5 ML model | 4-6 | 20-30 |
| A.6 Community + cloud | 3-4 | 15-20 |
| A.7 Production validation | 4+ | 20-30 |
| **Total to v1 production** | **22-30 weeks** | **110-160 sessions** |

5-7 month investment. Your **fourth primary stream** alongside audio (done), AOT (Phase 3 ready), networking (Δ.1 active), audit (read-only). Audit is read-only / opportunistic; configurator is full ownership.

---

## 12. Coordination touchpoints

### With Codex
- Profile schema includes HyperBridge env vars — must stay in sync
- New env knobs Codex adds → must add к profile schema
- Coordinate via reading Codex's recent commits monthly

### With your other streams
- **Audio**: profile field "audio_backend" maps to MACRUNNER_AUDIO_BACKEND
- **AOT cache**: profile field "aot_cache_mode" maps to MACRUNNER_HB_AOT_CACHE
- **Graphics**: profile field "gfx_backend" maps to MACRUNNER_GFX_BACKEND
- **Networking**: profile field "net_backend" maps to MACRUNNER_NET_BACKEND
- **Cloud (networking Phase Δ.5)**: A.6 uses /profiles/* endpoints — align schemas

### With Timur
- Strategic positioning (when ship AI feature, pricing tier)
- Anti-cheat policy decisions
- Brand / marketing for AI feature
- Legal review of community sharing terms

---

## 13. The vision

When done, MacRunner UX:

```
User drags Windows .exe onto MacRunner Dock icon
   ↓
[3 seconds]
   ↓
App opens, looks native, works first time
```

vs current Wine/Mythic UX:

```
User installs Wine
   ↓
User reads forums for hours
   ↓
User configures wine-staging vs wine-stable, tries 5 prefixes
   ↓
User tries winetricks combinations
   ↓
[some days later]
   ↓
App might work
```

This is **the experience** что makes MacRunner worth paying for. Wine alone = free + clunky. MacRunner = paid + magical.

**Commercial impact**: $9.99-$24/seat × 100k users in first year = $1-2.4M revenue. Defensible because competitors need months/years to replicate.

---

## Appendix A — Path quick reference

```
Repo:                    /Volumes/MacOS/MacRunner
AI subsystem workspace:  app/configurator/ai/
Templates workspace:     profiles/templates/
Docs:                    docs/AI-CONFIGURATOR-*.md
Progress log:            docs/KIMI-PROGRESS-ai-configurator.md
```

## Appendix B — Sanity check first

```bash
cd /Volumes/MacOS/MacRunner
. config/env.sh
echo "Root: $MACRUNNER_ROOT"

# Verify configurator exists
wc -l app/configurator/*.py | tail -5
ls profiles/

# Verify PE analyzer foundation
python3 -c "import lief; print(lief.__version__)" 2>&1
# If lief not installed: pip3 install --user lief

# Test existing configurator
cd app/configurator
python3 -c "from compatibility import *; print('compat OK')" 2>&1
```

---

End of brief. **You own AI Auto-Configurator from now until v1 production.**

5-7 months of commercial-tier work. **The strategic moat of MacRunner.**

Apply Zeroth Principle relentlessly. Build the experience that makes MacRunner worth paying for.
