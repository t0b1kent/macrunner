# Kimi MEGA Directive v2 — Cloud Backend + Unified Compatibility Pipeline

**Core mandate**: Build the **server-side infrastructure** that makes all your prior work shippable: cloud backend hosting compatibility profiles, visual regression baselines, AOT cache CDN, telemetry aggregation. Plus unify Configurator MVP + Visual Regression Lab + AI model into a single automated app-onboarding pipeline.

**Duration**: 2-4 months focused work.

**This is useful infrastructure, NOT cosmetic.** It's the commercial moat realized as working backend.

**Conflict risk**: ZERO с Codex (Codex on engine/Notepad++ functional). Your work pure backend + Python pipeline.

---

## 0. What you've already built (massive — recap)

- **Audio stack** — AVAudioEngine, 12/12 standalone tests
- **AOT cache** — Phase 1+2, runtime helpers, telemetry
- **Networking** — Δ.0-Δ.5: NWConnection, DNS, TLS, HTTP/URLSession, socket lifecycle. 26+ tests.
- **AI Configurator** — A.0-A.8: PE analyzer (92-dim), profile engine, error parser, auto-tuner, XGBoost model (v1.3, 100% Notepad++ accuracy), telemetry/feedback loop
- **Visual Regression Lab v1** — automated black-band/UI artifact detection (commit 9dc63c7)
- **AI Configurator MVP v0.1** — unified orchestrator, Codex task routing (commit 191ccf1)
- **Engine audits** — dim 02/06/07/08/09 findings forwarded к Codex

**This is shipping-grade engineering across 5 subsystems.** Now: tie it together с cloud backend.

---

## 1. Why cloud backend is the keystone

Right now everything is **local/standalone**:
- AI Configurator predicts profiles → but no community sharing
- Visual Regression Lab detects artifacts → but no baseline database
- AOT cache works → but no CDN distribution
- Telemetry collects → but no aggregation server
- Networking HTTP/TLS ready → but nothing to connect to

**Cloud backend unifies all this.** It's the missing piece что turns 5 standalone subsystems into **a product**:

```
User installs Windows app
  ↓
Configurator analyzes PE → queries CLOUD for community profile
  ↓ (cloud backend serves profile)
App runs → Visual Regression Lab checks vs CLOUD baseline
  ↓ (cloud serves expected visual baseline)
AOT cache misses → fetch precompiled blocks from CLOUD CDN
  ↓ (cloud serves AOT blocks)
App works → telemetry uploaded to CLOUD (anonymized)
  ↓ (cloud aggregates → improves model → benefits all users)
```

Without cloud — 5 nice local tools. With cloud — **defensible commercial moat** (community data network effect).

---

## 2. Phased work

### Phase Κ.0 — Architecture + design (1 week)

Per your methodology — design first.

Deliverables:
- `docs/CLOUD-BACKEND-ARCHITECTURE.md` — full system design, all endpoints
- `docs/CLOUD-BACKEND-ADR-001-stack.md` — Rust (axum) + Postgres confirmed (per Networking ADR earlier)
- `docs/CLOUD-BACKEND-ADR-002-storage.md` — profile/baseline/AOT blob storage strategy
- `docs/CLOUD-BACKEND-ADR-003-privacy.md` — GDPR (per earlier ADR-006), anonymization, retention
- `docs/CLOUD-BACKEND-OPENAPI.yaml` — full API spec

### Phase Κ.1 — Core API server (3-4 weeks)

Rust + axum + Postgres. Endpoints:

**Profiles**:
- `GET /v1/profiles/{pe_hash}` — fetch community profile
- `GET /v1/profiles/popular` — top 100 (cacheable)
- `POST /v1/profiles/contribute` — user uploads working profile (opt-in)
- `GET /v1/profiles/predict` — server-side XGBoost prediction (heavier model than client)

**Visual baselines**:
- `GET /v1/baselines/{app_hash}/{screen_id}` — expected visual reference
- `POST /v1/baselines/contribute` — Windows VM baseline upload

**AOT cache CDN**:
- `GET /v1/aot/{module_hash}/manifest` — available blocks
- `GET /v1/aot/{module_hash}/{block_hash}.bin` — block download

**Telemetry**:
- `POST /v1/telemetry/run` — anonymized run outcome (opt-in)
- `POST /v1/telemetry/crash` — crash report (opt-in)

**Auth**:
- Sign in with Apple (per earlier ADR)
- Anonymous tier (no login, fetch only)

Tests: API contract tests, load tests (1000 req/s), privacy audit.

### Phase Κ.2 — Database + storage (2 weeks)

- Postgres schema (profiles, baselines, telemetry aggregates, users, trust scores)
- Blob storage (S3-compatible: Cloudflare R2 / Bunny / MinIO local dev)
- Migration scripts
- Backup strategy

### Phase Κ.3 — Unified pipeline orchestrator (3-4 weeks)

Extend your Configurator MVP + Visual Regression Lab into ONE automated pipeline:

```
mr-onboard <app.exe>:
  1. PE analysis (your analyzer)
  2. Cloud profile lookup (Phase Κ.1)
  3. If no profile → XGBoost predict + cloud predict
  4. Apply profile, launch app via canonical wrapper
  5. Visual Regression Lab check vs cloud baseline
  6. Auto-tuner if issues (your auto-tuner)
  7. Functional smoke (extend Codex's harness)
  8. Generate Codex task if engine bugs found (your CODEX-NEXT-TASK routing)
  9. On success → contribute profile + baseline to cloud
  10. Telemetry upload
```

This is **the AI moat as automation**. Drop any .exe → auto-onboarded → community benefits.

### Phase Κ.4 — Client integration (2-3 weeks)

Wire MacRunner Configurator to use cloud:
- Use your Networking Δ.4 HTTP/URLSession client
- Profile fetch on app install
- AOT block prefetch
- Telemetry upload (opt-in)
- Visual baseline sync

### Phase Κ.5 — Deployment + ops (2 weeks)

- Single VPS deploy (Hetzner/DigitalOcean, ~$20/mo)
- CDN for static blobs (Cloudflare R2)
- CI/CD (GitHub Actions)
- Monitoring, logging
- Privacy compliance verification

### Phase Κ.6 — Validation (ongoing)

- Onboard 20+ real apps through full pipeline
- Measure: profile accuracy, visual detection rate, AOT speedup, time-to-onboard
- Document per-app в `docs/CLOUD-PIPELINE-VALIDATION-{app}.md`

---

## 3. Continue parallel — Networking Δ.6 IS this cloud backend

Your Networking brief Phase Δ.6 was "Cloud server backend". **This directive IS that phase, expanded.** They merge:
- Networking Δ.6 = the server (Phase Κ.1-Κ.2 here)
- Plus pipeline orchestration (Κ.3)
- Plus client integration (Κ.4)

So this isn't a NEW stream — it's your Networking primary stream's biggest phase, unified с your AI Configurator + Visual Lab work.

After cloud backend → Networking Δ.7 polish (Bonjour, SMB, per-app VPN) remains, smaller.

---

## 4. What stays standby

- **Audio E2E** — waits Codex Notepad++ functional (Wine stable)
- **AOT Phase 3 mechanical insertion** — waits Codex merge window (но cloud CDN part you can build now)
- **Graphics e2e** — waits Codex

When Codex closes Phase H Notepad++ functional → these unblock, you flash-close them (~1 session each), return to cloud.

---

## 5. Engine audit — opportunistic background

Remaining dims: 01 (memory model), 03 (decoder coverage), 04 (lifter/IR), 05 (JIT/interp consistency), 10 (test gaps).

Pick up opportunistically когда cloud work has natural pauses (waiting on builds, design reviews). Forward findings к Codex.

**Highest value remaining**: dim 05 (JIT vs interp consistency) — given Codex doing heavy opcode family work, divergence between JIT и interp paths could cause subtle bugs. Audit when time permits.

---

## 6. Methodology (unchanged — your discipline is excellent)

- Zeroth Principle: native, root-cause, seamless
- Design first (ADRs), code second
- Patch-by-evidence
- Decision autonomy max — escalate only commercial/legal/brand/partnership
- Honest reporting (your "no actionable findings" in dim 07 was exemplary)

---

## 7. ccache — use it

Verify your builds use ccache (currently 0% hit rate project-wide):
```bash
. config/env.sh
echo $CCACHE_DIR
ccache -s
```
For Rust — use `sccache` (Rust-native ccache equivalent):
```bash
brew install sccache
export RUSTC_WRAPPER=sccache
```

---

## 8. Why this is the right next mega-task

- **Useful, not cosmetic**: pure backend infrastructure + automation
- **Unifies your 5 subsystems** into shippable product
- **The commercial moat**: community data network effect
- **Independent of Codex**: zero conflict, parallel
- **Months of work**: cloud server + pipeline + deployment + validation
- **Leverages everything you built**: Configurator, Visual Lab, AOT, telemetry, networking client

When done: MacRunner has **working cloud backend** + **automated app onboarding pipeline**. Any .exe → analyzed → configured → tested → community-shared. **This is what competitors don't have and can't quickly replicate.**

---

## 9. First session

1. Read AGENTS.md fresh
2. Read this directive + KIMI-CONTEXT-RESTORE.md
3. Review your existing work: Configurator MVP, Visual Regression Lab, Networking client
4. Create `docs/CLOUD-BACKEND-ARCHITECTURE.md` skeleton
5. First 3 ADRs (stack, storage, privacy)
6. `docs/KIMI-PROGRESS-cloud.md` first entry
7. NO server code yet — design first

Поехали. Build the backend that makes everything shippable. Useful infrastructure. The moat.
