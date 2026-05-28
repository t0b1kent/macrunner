# Kimi MEGA Master Brief — Networking + Cloud Sync Stack

**Scope**: Replace Wine's POSIX socket emulation with native macOS Network.framework throughout MacRunner. Build cloud sync infrastructure for bottles/profiles/AOT cache distribution. Own full networking subsystem от network primitives до cloud backend.

**Duration**: 4-6 months focused work, 8 phases.

**Autonomy level**: Maximum. You own architectural decisions, implementation, server backend, testing, validation, documentation.

**Conflict risk with Codex**: ZERO. Networking entirely separate from HyperBridge/Wine ntdll/kernelbase/win32u. No merge conflicts ever.

**Conflict risk with other Kimi streams**: ZERO. Independent from audio (`engine/audio/`), AOT cache (`engine/hyperbridge/cache/`), graphics (`engine/graphics/`).

**Strategic value**: Critical foundation для productivity apps requiring network (1С cloud, AutoCAD licensing, Steam, Battle.net, any cloud SaaS app). Foundation для commercial features (cloud bottle sync, profile distribution, AOT cache CDN).

---

## 0. Mandatory reading

1. [/Users/timurtoby/Documents/MacRunner/Main/MacRunner/AGENTS.md](../AGENTS.md) — **🎯 Zeroth Principle** especially "native by default", все mandatory protocols
2. Your previous briefs (audio, AOT, graphics) — methodology pattern, same here
3. This brief
4. Apple Network.framework docs: https://developer.apple.com/documentation/network

---

## 1. Strategic context

### Why networking is critical

Look at the app ladder ([92-strategic-app-ladder-roadmap.md](file:///Users/timurtoby/Documents/MacRunner/92-strategic-app-ladder-roadmap.md)):

| App | Network dependency |
|---|---|
| Notepad++ | None (text editor) |
| KeePass 1.43 | None |
| KeePass 2.x | Optional sync to cloud |
| **1С Тонкий клиент** | **Essential — entire app is client to server** |
| **AutoCAD LT** | **License check on startup, cloud features** |
| **Revit** | License + cloud collaboration |
| **Photoshop** | License + Creative Cloud sync |
| **Navisworks** | License + cloud BIM models |
| Modern games | License + multiplayer + DRM |

**Without proper networking, MacRunner can't serve any commercial productivity app**. License servers fail, cloud sync breaks, multiplayer doesn't work, online activation hangs.

### Current state: Wine POSIX shim

Wine's networking implementation:
```
Windows app → WinSock2 (ws2_32.dll)
  → Wine's WinSock implementation
    → POSIX socket() / connect() / send() / recv()
      → BSD sockets layer
        → macOS XNU kernel
```

Problems с этим:
- **Not Mac-native**: doesn't integrate с macOS preferences (proxy, VPN, content filter)
- **Performance penalty**: extra translation layers
- **TLS handled twice**: Windows TLS stack inside Wine + BSD socket goes through SSL
- **Certificate store split**: Windows trusts ≠ macOS Keychain trusts
- **No QUIC / HTTP/3 native support**: BSD sockets don't have these primitives
- **Proxy detection broken**: doesn't read macOS System Preferences proxy config
- **VPN awareness missing**: macOS per-app VPN doesn't route Wine apps properly
- **Bonjour/.local resolution broken**: Wine uses /etc/resolver but не Bonjour
- **Captive portal detection missing**: macOS knows when network needs login, Wine doesn't

### Vision: Native macOS networking

Replace POSIX shim с Network.framework integration:
```
Windows app → WinSock2 (ws2_32.dll, kept)
  → Wine's WinSock — REWRITE to use:
    → Network.framework (NWConnection, NWListener, NWPath)
      → macOS unified networking stack
        → Hardware (incl. NIC offloads, encryption acceleration)
```

Result:
- Native macOS integration (proxy, VPN, captive portal все работают automatically)
- TLS unified (single trust store)
- QUIC / HTTP/3 supported via Network.framework
- Bonjour / mDNS automatic
- Better performance (offloads, encryption acceleration on Apple Silicon)
- Per-app permissions (macOS asks user "this Windows app wants network", Camera-style)

### Cloud sync layer

Beyond per-app networking, build product-level cloud features:
- **Bottle sync**: user's bottles stored in iCloud Drive (или нашем backend), available across Macs
- **Profile distribution**: community profiles for popular apps delivered from our server
- **AOT cache CDN**: pre-compiled blocks for popular apps downloaded на first install
- **Telemetry**: privacy-respecting crash reports, perf metrics
- **License sync**: user's MacRunner license active across devices

This is **commercial product infrastructure**. Cannot ship paid product без этого.

---

## 2. Current state — what exists

### Wine network DLLs (exist, work через POSIX)
- `ws2_32` — WinSock2 core
- `wsock32` — older WinSock
- `mswsock` — extensions
- `dnsapi` — DNS
- `iphlpapi` — IP helpers
- `netapi32` — SMB/NetBIOS
- `wininet`, `winhttp`, `urlmon` — HTTP stack
- `secur32`, `crypt32`, `bcrypt` — crypto/TLS
- `dpwsockx` — DirectPlay

### MacRunner specific
- No native networking integration yet
- No cloud sync infrastructure
- No backend server
- No telemetry

### What you're starting from

Greenfield для native integration. Wine baseline works для simple cases (HTTP fetch, simple TCP), but lacks Mac integration.

---

## 3. Phased work plan

### Phase Δ.0 — Audit, strategy, design (1-2 weeks)

Like graphics, audio, AOT — audit first, design second, code third.

Deliverables:
- `docs/NETWORKING-AUDIT.md` — current state, Wine's POSIX shim layout, gaps vs macOS native
- `docs/NETWORKING-STRATEGY.md` — architecture diagram, decision: which Wine DLLs rewrite, which leave POSIX, transition plan
- `docs/NETWORKING-ROADMAP.md` — Phase Δ.1 through Δ.7 timeline
- `docs/NETWORKING-ADR-001-native-vs-shim.md` — first ADR: replace Wine TCP/UDP layer entirely, or wrap?
- `docs/NETWORKING-ADR-002-tls-strategy.md` — second ADR: Apple SecureTransport vs Wine's bcrypt vs Network.framework's TLS
- `docs/NETWORKING-ADR-003-certificate-trust.md` — third ADR: Keychain integration policy

### Phase Δ.1 — TCP/UDP via Network.framework (3-4 weeks)

Replace BSD socket calls с NWConnection:

1. New native side library: `engine/networking/macos/native/`
   - `nw_connection.m` — NWConnection wrapper
   - `nw_listener.m` — NWListener (server sockets)
   - `nw_path.m` — NWPathMonitor (network reachability)
   - `nw_bridge.c` — C ABI for Wine to call

2. Wine `ws2_32` DLL — modify Unix side to optionally use native:
   - Env knob: `MACRUNNER_NET_BACKEND={posix,nw,auto}`
   - Posix остаётся default initially; nw opt-in for testing
   - Auto switches к nw on macOS 14+

3. Per-socket attributes:
   - Bind to specific interface (для VPN routing)
   - TLS via NWProtocolTLS
   - QUIC option
   - Connection tracking via NWConnection state

4. Tests:
   - Unit: NWConnection lifecycle, error handling
   - Integration: simple HTTP fetch in Windows app, verify routes through nw_bridge
   - Compatibility: existing Wine HTTP tests pass с nw backend

Success: HTTP fetch in test Windows app works через `MACRUNNER_NET_BACKEND=nw`, traffic visible в Console.app native, proxy/VPN respected.

### Phase Δ.2 — DNS via macOS resolver (2 weeks)

Replace Wine's `dnsapi` POSIX resolver:

1. New: `engine/networking/macos/dns/`
   - Use `dnssd` API (Bonjour) for `.local` resolution
   - Use `nw_path_get_dns_servers()` для system DNS
   - Use `DNSServiceQueryRecord()` для async queries
   - Support: A, AAAA, MX, SRV, TXT records

2. Wine `dnsapi` Unix side rewrite — call our native resolver

3. Special handling:
   - `.local` domains → Bonjour automatically
   - DNS-over-HTTPS если configured в macOS
   - VPN-routed DNS если VPN active

4. Tests:
   - Resolve standard domains (google.com)
   - Resolve `.local` (printer.local)
   - Honor /etc/hosts override

Success: Windows app resolving `apple.com` routes through Apple's native DNS, including any user-configured DNS-over-HTTPS.

### Phase Δ.3 — TLS via SecureTransport / Network.framework (3-4 weeks)

Replace Wine's `schannel` / `bcrypt` TLS stack для outgoing connections:

1. New: `engine/networking/macos/tls/`
   - `tls_native.m` — Network.framework TLS protocol options
   - Cipher suite negotiation
   - Certificate chain validation via macOS Keychain
   - Client certificate support

2. Wine `secur32`, `schannel` — rewrite Unix side для использования native TLS

3. Trust integration:
   - Server cert validated против macOS Trust Store
   - User-installed certificates honored
   - Pinning policies (если app provides via WinAPI)

4. Client cert support:
   - macOS Keychain provides client identity
   - Smart card support (if user has YubiKey/CAC reader)

5. Tests:
   - HTTPS to standard sites
   - Self-signed cert (should fail with proper error)
   - Server requesting client cert
   - Cipher suite enumeration matches macOS defaults

Success: Windows apps using TLS see macOS-issued certificates, same trust decisions as Safari.

### Phase Δ.4 — HTTP stack: WinHTTP/WinINet → URLSession bridge (3-4 weeks)

Replace Wine's POSIX-based HTTP implementations:

1. New: `engine/networking/macos/http/`
   - `urlsession_bridge.m` — URLSession wrapper
   - Configuration: cookies, credentials, cache, timeouts
   - Auto-handles proxy from macOS Preferences
   - HTTP/2, HTTP/3 (QUIC) automatic

2. Wine `wininet`, `winhttp`, `urlmon` Unix side rewrite

3. Cookies:
   - URLSession's NSHTTPCookieStorage — system-wide
   - Per-app isolation: separate NSHTTPCookieStorage per Wine prefix

4. Authentication:
   - Basic, Digest auto-handled
   - NTLM via system support (если configured)
   - Kerberos (если user has tickets)

5. Tests:
   - Simple GET, POST
   - Cookies persisted across requests
   - HTTP/2 multiplexing
   - HTTP/3 (на supported networks)
   - Proxy authentication

Success: 1С Тонкий клиент successfully connects к 1С cloud (uses HTTPS POST extensively).

### Phase Δ.5 — Cloud infrastructure backend (4-6 weeks)

Build server-side для cloud features. **This is a separate project** within MacRunner repo:

1. New: `cloud/` directory at repo root
2. Tech stack decision (your ADR):
   - Rust (axum/actix) — best perf, modern, but learning curve если new
   - Go (gin/echo) — simple, fast, easy operations
   - Node.js — fast dev, but slower runtime
   - Recommend: **Rust** for production-grade
3. Components:
   - `cloud/api/` — REST API server
   - `cloud/db/` — schema (Postgres recommended)
   - `cloud/cdn/` — static asset hosting (profiles, AOT cache, bottle templates)
   - `cloud/telemetry/` — ingestion endpoint, privacy-respecting
   - `cloud/auth/` — Sign-in with Apple primary, optional email/password
4. Endpoints:
   - `GET /profiles/popular` — community profiles for top apps
   - `GET /profiles/{app-hash}` — specific app profile
   - `POST /profiles/contribute` — user submits working profile (manual review queue)
   - `GET /aot-cache/{app-hash}/manifest` — list available blocks
   - `GET /aot-cache/{app-hash}/{block-hash}.bin` — block download (CDN)
   - `POST /telemetry/crash` — crash reports (opt-in)
   - `POST /telemetry/perf` — performance metrics (opt-in, aggregated)
   - `GET /licenses/{user}` — user's MacRunner license state
   - `POST /bottles/sync` — bottle metadata sync (actual bottle content в iCloud Drive)
5. Privacy:
   - Telemetry opt-in, never collected without consent
   - No PII unless user explicitly logs in
   - Aggregated metrics только, no per-user logs unless crash report
   - GDPR compliant deletion endpoint
   - Hosted in EU + US, user choice
6. Deployment:
   - Initial: single VPS (Hetzner, DigitalOcean, $50/mo)
   - Scale path: CDN (Cloudflare, Bunny) for static assets, regional API replicas
   - CI/CD: GitHub Actions deploy
7. Tests:
   - API contract tests
   - Load tests (1000 req/s)
   - Privacy audit checklist

Success: server running, API endpoints functional, MacRunner client connecting и fetching profiles.

### Phase Δ.6 — Client-side cloud features (3-4 weeks)

Wire MacRunner Configurator / Control Center to use cloud:

1. New: `engine/networking/cloud/`
   - `cloud_client.m` — Swift/ObjC client для our cloud API
   - `profile_sync.m` — bottle profile sync
   - `aot_cache_fetcher.m` — block downloader
   - `telemetry_uploader.m` — opt-in telemetry submission

2. Configurator integration:
   - On install of new Windows app: fetch profile from cloud
   - If profile updated: prompt user "Updated profile available"
   - If user fixes own profile: prompt "Share with community?"

3. AOT cache integration (after Kimi's AOT work integrates):
   - On install: fetch cache manifest, pre-download top blocks
   - On run: any missed local cache → fetch from CDN before JIT

4. Telemetry (opt-in):
   - Crash → upload (after user permission)
   - Slow start time → aggregated metric
   - Black screen → diagnostic capture
   - Always anonymous, hashed identifiers

5. License sync:
   - Sign-in with Apple
   - License token cached locally
   - Auto-refresh
   - Multi-Mac support (up to N devices per license)

6. Tests:
   - Mock cloud server для testing
   - Offline behavior (queue uploads, graceful degradation)
   - Privacy: nothing uploaded без consent

Success: MacRunner Configurator fetches profile from cloud для new app install, AOT blocks pre-downloaded, optional telemetry working.

### Phase Δ.7 — Polish, performance, advanced features (4+ weeks)

- **Bonjour discovery** для local services (printers, file shares discovered automatically)
- **SMB native** via NetFS framework (vs Wine's netapi32 POSIX)
- **Captive portal handling** — macOS API tells us network needs login, surface to user
- **Per-app VPN** — Mac users can route specific Windows app через VPN
- **Network condition simulation** для testing (low bandwidth, high latency)
- **WebSocket optimization** — Network.framework has native WebSocket
- **HTTP/3 (QUIC)** — works automatically via Network.framework
- **WireGuard integration** (если user has it configured)
- **Tor support** (через macOS Tor configuration)

---

## 4. Architectural decisions (your authority)

Per AGENTS.md Decision Rubric — you decide and document:

- **Native vs shim**: replace Wine POSIX entirely or layer над POSIX? Default: replace ws2_32 Unix side, leave POSIX as fallback via env knob.
- **Async model**: NWConnection blocks are async by design; how bridge к synchronous WinSock recv()? Likely thread per pending recv, or completion-based с GCD.
- **Server tech stack**: Rust/Go/Node? Your choice in ADR-100 series.
- **Auth**: Sign-in with Apple primary? Email/password fallback? OAuth2 with provider?
- **Telemetry retention**: 30 days? 90 days? Aggregated forever?
- **Profile review**: auto-publish or manual moderation? Spam prevention?
- **CDN provider**: Cloudflare R2 / Bunny / direct S3? Cost-perf tradeoff.
- **Database**: Postgres / SQLite for v1? Migrate to scale later?

Escalate (Questions for Timur):
- Legal: GDPR specifics, data residency
- Commercial: pricing tier for telemetry/sync features
- Partnership: hosting provider negotiations
- Brand: server naming, public URLs

---

## 5. Workspace

```
/Users/timurtoby/Documents/MacRunner/Main/MacRunner/
├── engine/networking/                            # YOUR PRIMARY workspace
│   ├── macos/native/                              # Network.framework wrappers
│   ├── macos/dns/                                 # DNS via macOS resolver
│   ├── macos/tls/                                 # TLS via SecureTransport
│   ├── macos/http/                                # URLSession bridge
│   ├── cloud/                                     # client side cloud features
│   ├── tests/                                     # unit + integration
│   └── docs/                                      # internal design docs
├── cloud/                                         # SERVER backend (separate)
│   ├── api/                                       # REST API
│   ├── db/                                        # schema
│   ├── cdn/                                       # static assets
│   ├── telemetry/                                 # ingestion
│   ├── auth/                                      # Sign-in with Apple
│   ├── deploy/                                    # CI/CD, infra-as-code
│   └── tests/                                     # API contract tests
├── scripts/
│   ├── build-networking-macos.sh
│   ├── build-cloud-server.sh
│   ├── deploy-cloud.sh
│   └── run-networking-test-*.sh
├── docs/
│   ├── NETWORKING-AUDIT.md
│   ├── NETWORKING-STRATEGY.md
│   ├── NETWORKING-ROADMAP.md
│   ├── NETWORKING-BUILD.md
│   ├── NETWORKING-ADR-{NNN}-{title}.md            # multiple ADRs
│   ├── NETWORKING-VALIDATION-{app}.md             # per-app validation
│   ├── CLOUD-API-SPEC.md                          # OpenAPI spec
│   ├── CLOUD-DEPLOY.md
│   ├── CLOUD-PRIVACY.md
│   └── KIMI-PROGRESS-networking.md                # YOUR PROGRESS LOG
└── artifacts/networking-test/                    # test apps, captures
```

### Do NOT touch

- `engine/hyperbridge/` — Codex active work + your AOT cache в `cache/` subdir
- `engine/wine/dlls/ntdll/`, `kernelbase/`, `win32u/` — Codex
- `engine/audio/`, `engine/wine/dlls/winecoreaudio.drv/` — your audio (separate stream)
- `engine/graphics/`, `engine/dxmt/`, `engine/dxvk/`, `engine/vkd3d/`, `engine/moltenvk/` — graphics done

Wine network DLLs (`ws2_32`, `wininet`, etc.) are SHARED — you'll modify their Unix-side build to plug in our native backend, but don't rewrite PE-side (Windows API contract).

---

## 6. Methodology (same as previous briefs)

### Zeroth principle (mandatory)
- Native by default: Network.framework first, POSIX fallback only
- Root-cause every bug found in audit
- No workarounds without TODO + tracked subtask
- Performance equal или better than Wine POSIX shim

### Family audit
- Found one socket option ignored → audit ВСЕ socket options
- Found one TLS cipher mishandled → audit cipher suite negotiation full
- Found one DNS record type missing → audit all record types

### Patch-by-evidence
- Real network captures (Wireshark, Charles Proxy) before claiming "works"
- macOS Console.app native logs visible
- Performance numbers measured (latency, throughput)

### Decision autonomy
- Most decisions yours, документируй в ADR
- Escalate только: legal/GDPR, commercial pricing, partnership negotiations

---

## 7. Communication

### Progress log: `docs/KIMI-PROGRESS-networking.md`

```markdown
## YYYY-MM-DD — Phase Δ.X, Session N

What was done:
- [bullet points]

Decisions made (per autonomy):
- [с ADR link если significant]

Tests/validation:
- [what verified, what numbers, captures]

Blockers / Questions for Timur:
- [TRUE blockers only]

Next session plan:
- [bullet points]
```

### Per-phase closeout
`docs/NETWORKING-PHASE-Δ-X-CLOSURE.md` с deliverables, perf numbers, known issues, recommendation.

### ADRs prolifically
Network architecture has many decisions. Write ADRs for any non-trivial choice. They survive context compaction perfectly.

---

## 8. Success criteria per phase

### Phase Δ.1 success
- [ ] NWConnection wrapper builds, codesigns, loads
- [ ] Simple HTTP fetch in Windows app routes через Network.framework
- [ ] Proxy from macOS Preferences honored
- [ ] VPN routing works (if user has VPN configured)
- [ ] Wine compat tests pass с new backend

### Phase Δ.2 success
- [ ] DNS queries route через native resolver
- [ ] `.local` (Bonjour) resolves
- [ ] DNS-over-HTTPS honored if configured
- [ ] /etc/hosts overrides work

### Phase Δ.3 success
- [ ] HTTPS to standard sites validates через Keychain trust
- [ ] User-installed certs honored
- [ ] Client certs from Keychain supported
- [ ] Modern cipher suites only (no TLS 1.0/1.1)

### Phase Δ.4 success
- [ ] WinHTTP/WinINet via URLSession works
- [ ] HTTP/3 (QUIC) on supported networks
- [ ] Cookies persist correctly
- [ ] 1С Тонкий клиент connects to cloud successfully

### Phase Δ.5 success
- [ ] Cloud API server running, OpenAPI spec published
- [ ] All endpoints functional, tests passing
- [ ] Privacy audit passed
- [ ] Deployed to production VPS, accessible at api.macrunner.dev (or chosen URL)

### Phase Δ.6 success
- [ ] Configurator fetches profile from cloud on new app
- [ ] AOT cache CDN delivers blocks
- [ ] Telemetry uploads correctly с opt-in
- [ ] License sync works across devices

### Phase Δ.7 success
- [ ] Bonjour discovery automatic
- [ ] SMB native shares mount
- [ ] Captive portal surfaced to user
- [ ] Per-app VPN routing functional

---

## 9. Reference materials

### Apple frameworks
- Network.framework: https://developer.apple.com/documentation/network
- URLSession: https://developer.apple.com/documentation/foundation/urlsession
- SecureTransport: deprecated, replace с Network.framework's TLS
- NetFS: SMB/AFP file shares
- DNSServiceDiscovery (Bonjour): https://developer.apple.com/documentation/dnssd

### Wine internals
- `engine/wine/dlls/ws2_32/` — WinSock2 Unix-side
- `engine/wine/dlls/wininet/`, `winhttp/` — HTTP stacks
- `engine/wine/dlls/secur32/`, `schannel/` — TLS
- `engine/wine/dlls/dnsapi/` — DNS

### Cloud server references
- Rust web frameworks: axum (recommended), actix-web
- Database: Postgres + sqlx (Rust)
- Authentication: Sign in with Apple — https://developer.apple.com/sign-in-with-apple/
- Hosting: Hetzner CX series ($5-20/mo VPS), DigitalOcean
- CDN: Cloudflare R2 + Workers, Bunny CDN

### Testing tools
- Wireshark — packet capture
- Charles Proxy — HTTPS interception (for debugging)
- mitmproxy — automated testing
- iperf — bandwidth measurement
- Apple's Network Link Conditioner — simulate slow networks

---

## 10. First-session goals

Session 1 — read + audit, no code:

1. Read AGENTS.md, especially Zeroth Principle
2. Read this brief fully
3. Inspect existing state:
   ```bash
   ls engine/wine/dlls/ | grep -iE 'ws2|wsock|wininet|winhttp|secur|crypt|dns|iphlp|netapi'
   ls engine/networking/ 2>/dev/null  # likely doesn't exist yet
   ls cloud/ 2>/dev/null  # likely doesn't exist yet
   ```
4. Read Wine's `dlls/ws2_32/unixlib.c` (or equivalent) — understand current POSIX shim
5. Create skeletons:
   - `docs/NETWORKING-AUDIT.md`
   - `docs/KIMI-PROGRESS-networking.md` (first entry)
6. Plan Session 2: complete audit, start Strategy doc

Session 2-3: complete Phase Δ.0 audit + strategy + roadmap + first ADRs.

Session 4+: begin Phase Δ.1 NWConnection wrapper.

---

## 11. Timeline expectations

| Phase | Weeks | Sessions |
|---|---|---|
| Δ.0 Audit & strategy | 1-2 | 5-8 |
| Δ.1 TCP/UDP via Network.framework | 3-4 | 15-20 |
| Δ.2 DNS native | 2 | 8-10 |
| Δ.3 TLS native | 3-4 | 12-15 |
| Δ.4 HTTP via URLSession | 3-4 | 15-20 |
| Δ.5 Cloud server backend | 4-6 | 25-30 |
| Δ.6 Client cloud features | 3-4 | 15-20 |
| Δ.7 Polish & advanced | 4+ | 20+ |
| **Total to v1 production** | **22-30 weeks** | **115-145 sessions** |

This is 5-7 month investment. You're primary networking + cloud engineer. Full ownership.

---

## 12. Coordination touchpoints

### With Codex
- 95% independent
- Rare: if Wine network DLL needs internal restructuring (rare), Codex coordinates

### With your other streams
- **Audio**: independent
- **AOT cache**: Phase Δ.5 cloud backend hosts AOT block CDN — natural integration
- **Graphics**: independent (different DLLs)

### With Timur
- ADRs for major decisions
- Legal/GDPR escalations
- Commercial pricing decisions
- Server hosting cost approval
- Brand/URL decisions

---

## 13. The vision

When you're done:

- Every Windows app's network traffic routes через native macOS stack
- Performance equal или better than Wine POSIX shim
- 1С, AutoCAD, Photoshop activate licenses correctly
- Cloud server running, profiles distributed, AOT blocks delivered, telemetry collected
- Sign-in with Apple integrated
- HTTP/3, QUIC supported
- VPN, proxy, captive portal все работают seamlessly
- Bottles sync across user's Macs via iCloud Drive

This is **production infrastructure** для MacRunner commercial product. Cannot ship paid version без этого.

---

## Appendix A — Path quick reference

```
Repo root:              /Users/timurtoby/Documents/MacRunner/Main/MacRunner
Networking workspace:   engine/networking/
Cloud server:           cloud/
Wine network DLLs:      engine/wine/dlls/ws2_32/, wininet/, winhttp/, secur32/, dnsapi/, ...
Documentation:          docs/NETWORKING-*.md, docs/CLOUD-*.md
Progress log:           docs/KIMI-PROGRESS-networking.md
```

## Appendix B — Sanity check first

```bash
cd /Users/timurtoby/Documents/MacRunner/Main/MacRunner
. config/env.sh
echo "Root: $MACRUNNER_ROOT"
ls engine/wine/dlls/ | grep -iE 'ws2|wsock|wininet|winhttp|dnsapi|secur32' | sort
test -d engine/networking && echo "networking dir exists" || echo "networking dir not yet created"
test -d cloud && echo "cloud dir exists" || echo "cloud dir not yet created"
xcrun --show-sdk-version  # verify macOS SDK available
swift --version  # verify Swift toolchain for cloud backend if Rust not preferred
```

---

End of brief. **You own networking + cloud subsystem from now until production v1.**

5-7 months of strategic work. Apply Zeroth Principle relentlessly. Native macOS where possible.

Make MacRunner network-ready for 1С, AutoCAD, Photoshop, Navisworks, and beyond.
