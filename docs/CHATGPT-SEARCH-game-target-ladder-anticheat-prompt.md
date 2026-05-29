# ChatGPT (web search) — First-games target ladder + anti-cheat/driver compatibility filter (PASTE-READY)

> Вставь весь блок ниже в ChatGPT с web search. Самодостаточный, доступа к репо не нужно.

---

Product/market research for a Windows-games-on-Apple-Silicon translation layer. Context:
I'm building a commercial product that runs **x86_64 Windows games on Apple Silicon (ARM64)
macOS WITHOUT Rosetta** — my own x86_64→ARM64 CPU translator ("HyperBridge") + a pure-ARM64
fork of **Wine 11** + **ARM64EC** (system DLLs native ARM, game x64 code emulated by
HyperBridge) + a **DirectX 11 → Metal** translation layer (DXMT-style) with a native Metal
backend. Think CrossOver/Whisky/Game Porting Toolkit class, but without Rosetta.

I need a **prioritized "first games" target ladder** — which titles to make work first — and,
crucially, a **compatibility filter** so I don't waste weeks on games that are architecturally
impossible for a translation layer. Use web search; cite sources (ProtonDB, AreWeAntiCheatYet,
CodeWeavers/CrossOver compatibility DB, Whisky/GPTK reports, anti-cheat vendor docs, Steam
hardware/API data). Give versions/dates where compatibility changed.

## 1. The KILL FILTER (most important) — what is architecturally impossible / very hard
- **Anti-cheat:** Which anti-cheat systems are fundamentally incompatible or require explicit
  vendor opt-in for Wine/Proton/translation layers? Cover: Easy Anti-Cheat (EAC), BattlEye,
  Vanguard (Riot), Denuvo Anti-Tamper, nProtect GameGuard, Ricochet (CoD), Punkbuster. For
  each: does it use a **kernel-mode driver** (instant no on macOS/Wine)? Does it have a
  Proton/Wine opt-in path, and is that path usable OUTSIDE Steam/Proton? Cite
  AreWeAntiCheatYet status.
- **Kernel drivers / ring-0:** which games install kernel drivers (anti-cheat or DRM) that
  cannot exist under Wine on macOS — auto-exclude these.
- **API mismatch:** games that are **DirectX 12-only** or **Vulkan-only** (my layer is D3D11
  → Metal first). Flag DX12-only titles as "later/harder". Note which big titles are D3D11.
- **DRM:** Denuvo and always-online DRM behavior under translation layers.

## 2. The GREEN LIST — best first targets (ladder, easy → hard)
Rank ~15-20 concrete titles that are realistic early wins, each annotated with:
- Graphics API (D3D11 preferred for me; note if DX9/DX10/DX12).
- Anti-cheat status (ideally NONE, or single-player/offline).
- Whether it's known-good on CrossOver / Whisky / GPTK / Proton (cite).
- Engine (Unity / Unreal / proprietary) and rough demands (CPU-heavy vs GPU-heavy).
- Popularity/commercial pull (does making it work matter for marketing?).
Bias toward: single-player, no anti-cheat, D3D11, native Windows launcher (not heavy
Epic/EA/Ubisoft launcher dependency), and titles that are famous enough to be a marketing
proof-point ("look, X runs").

## 3. Tiering for a translation-layer bring-up
Group the green list into: **Tier 0** (tech-demo / smoke: tiny D3D11 samples, old indies),
**Tier 1** (real but light games, no anti-cheat), **Tier 2** (AAA single-player D3D11),
**Tier 3** (multiplayer / anti-cheat — only if vendor opt-in exists). For each tier name
2-4 specific titles.

## 4. Launcher / store reality
- Which storefront launchers (Steam, GOG Galaxy, Epic, EA app, Ubisoft Connect, Battle.net)
  work vs fight Wine? GOG offline installers vs Steam — which is easiest for bring-up?
- Note titles available DRM-free (GOG) as the cleanest first targets.

## OUTPUT
- A clear KILL-FILTER list (anti-cheat/kernel/DX12 → avoid), with reasons + sources.
- A ranked GREEN-LIST ladder (Tier 0→3) with the annotations above.
- A 5-line bottom-line recommendation: which 3-5 titles to target FIRST and why.
- "Open questions" for anything uncertain — mark unknown, do NOT guess.
- Cite every compatibility claim with a source + date (compat changes over time).
