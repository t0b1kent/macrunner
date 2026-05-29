# ChatGPT (web search) — Store-layer architecture: how Mythic/Heroic/Legendary decouple store from execution, and can it be done for Steam (PASTE-READY)

> Вставь весь блок ниже в ChatGPT с web search. Самодостаточный, доступа к репо не нужно.

---

Architecture research for a commercial Mac app that runs **Windows games on Apple Silicon
without Rosetta** (own x86_64→ARM64 translator + pure-ARM64 Wine 11 + DirectX11→Metal). I
already have the game-execution layer. Now I'm designing the **store/library layer** — how the
app authenticates, downloads, licenses and launches games from each storefront. Use web search;
cite sources (Heroic Games Launcher docs/GitHub, Legendary GitHub, Mythic/Whisky/CrossOver docs,
Lutris, SteamCMD docs, ProtonDB, Valve/Epic developer docs). Give versions/dates.

## Background premise (verify or correct me)
- Running the **Windows Epic Games Launcher under Wine is broken** (crashes on mandatory
  updates, login fails). Heroic/Mythic avoid it by using **Legendary** — a native
  (Python/CLI) reimplementation of the Epic client that runs natively on macOS/Linux, handles
  auth + downloads the **Windows build** of the game, and then only the game `.exe` runs under
  Wine. I.e. they **decouple the store layer (native) from game execution (Wine)**.
- Running the **Windows Steam client under Wine works** reasonably well (CrossOver/Whisky
  path); Steam DRM (`steam_api(64).dll`) is satisfied because the real Steam client is running.
- The Steam **Mac client only lets you install titles that have Mac depots** — to get the full
  Windows catalog on a Mac you currently run Windows Steam under Wine.

## What I need, precisely:

### 1. Epic / Legendary architecture (the model to copy)
- Exactly how does **Legendary** authenticate (OAuth flow with Epic?), fetch the manifest, and
  download the **Windows** build while running natively on macOS? What does it NOT do that the
  real launcher does (EOS overlay, achievements, cloud saves)?
- How does **Heroic** wrap Legendary + the Wine/Proton/GPTK runner? Where is the boundary —
  what runs native vs under Wine? Point to the repos/files.
- How does **Mythic** (Mac-native) do it — does it bundle Legendary + its own "Mythic Engine"
  (GPTK/Wine-based)? Cite.
- Which Epic games still FAIL this model and why (EOS-dependent / Epic Online Services,
  anti-cheat, kernel DRM)?

### 2. Can the same "native store client" model be built for STEAM? (the key question)
- Is there a **native (non-Wine) way to download Windows depots** of a Steam game on macOS,
  given Steam DRM? Options to evaluate: **SteamCMD** (does it run native on Apple Silicon? can
  it download Windows depots from a Mac? does it handle login/2FA?), DepotDownloader, or other.
- The DRM problem: many Steam games require the **running Steam client** for `steam_api` /
  Steamworks DRM at launch. Can a downloaded Windows game be launched under Wine WITHOUT a
  running Steam client? Which games are "DRM-free once downloaded" vs which hard-require the
  client? How do CrossOver/Whisky handle this today — do they always run Windows Steam under
  Wine, or is there a native-download + Wine-launch path?
- Is "Legendary for Steam" feasible at all, or is running **Windows Steam under Wine** the only
  realistic path? Give a clear verdict with sources.

### 3. GOG and others
- GOG is DRM-free — confirm: native download (GOG API / gogdl / Heroic's GOG support) + direct
  Wine launch, no client. What's the cleanest path?
- Amazon Games (nile), itch.io — briefly, are they viable native-store targets?

### 4. Per-store recommendation for MY app
Given my constraint (Apple Silicon, no Rosetta, own Wine+translator, want smoothest UX with the
LEAST stuff running under Wine), recommend the architecture per store:
- Steam: native-download-then-Wine vs Windows-Steam-under-Wine — which, and why.
- Epic: confirm Legendary-native model + integration notes.
- GOG: confirm native + direct launch.
- Which open-source components (Legendary, gogdl, nile, SteamCMD, DepotDownloader) I could
  reuse/embed, and their licenses (can I bundle them in a commercial app?).

## OUTPUT
- A per-store architecture table: store → store-layer (native? which tool?) → execution-layer
  (Wine) → DRM caveat → license of reusable OSS component.
- A clear verdict on the Steam question: is a native-download store layer possible, or must
  Windows Steam run under Wine?
- License notes for embedding Legendary/gogdl/nile/SteamCMD in a commercial product.
- "Open questions" for anything uncertain — mark unknown, do NOT guess.
- Cite every claim with a source + date (these tools change fast).
