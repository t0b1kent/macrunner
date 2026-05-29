# Store-layer architecture (Legendary / Steam / GOG) — ChatGPT web-search report

**Date:** 2026-05-29
**Source:** ChatGPT (web search), prompt = `docs/CHATGPT-SEARCH-store-layer-architecture-legendary-steam-prompt.md`
**Status:** Research closed. Per-store verdict + OSS-license matrix below.

> Full original report archived verbatim after the synthesis. Synthesis is operator-side.

---

## VERDICT (one paragraph)
The "decouple store-layer (native) from execution-layer (Wine)" model is **confirmed**.
Epic = best fit (Legendary native OAuth + Windows-depot download, only the `.exe` runs under
Wine). GOG = ideal (DRM-free, gogdl native, direct `.exe` launch, no client). **Steam = the
asymmetry you spotted is real**: a full "Legendary for Steam" is NOT buildable without DRM
bypass — most games require the *running* Steam client for `steam_api(64).dll`. SteamCMD /
DepotDownloader can download Windows depots natively, but only DRM-free games launch without
the client; the realistic Steam path stays **Windows Steam under Wine** (our ARM64-Wine + x64
translator). Amazon (nile) / itch.io (butler) = niche.

## Per-store matrix
| Store | Store layer (native?) | Execution | DRM/anti-cheat caveat | OSS / license |
|---|---|---|---|---|
| **Epic** | **Legendary** CLI (OAuth, manifests, Windows depots) | `.exe` under our Wine | no built-in DRM; EAC/BattlEye/Vanguard/Ricochet/GameGuard kernel = dead; EOS overlay/achievements need per-prefix activation, macOS support partial | Legendary / Heroic / Mythic all **GPL-3.0** |
| **Steam** | partial: SteamCMD (`@sSteamCmdForcePlatformType windows`) or DepotDownloader (`-os windows`, 2FA `-qr`/`-remember-password`) — **download only** | most games need **running Steam client** → run **Windows Steam under Wine**; DRM-free subset can launch direct | Steamworks DRM ties to account; DRM-free list small + unpublished; anti-cheats block Wine | SteamCMD = **proprietary Valve**; DepotDownloader = **GPL-2.0** |
| **GOG** | native via official API / **gogdl** | `.exe` under Wine, no client | DRM-free by policy | gogdl **GPL-3.0** |
| **Amazon** | **nile** | Wine | external DRM (Ubisoft/EA) often needs extra client | nile **GPL-3.0** |
| **itch.io** | **butler** | Wine | mostly DRM-free | butler **MIT** |

## ⚠️ Commercial-licensing landmine (the part that matters for shipping)
**Every serious store tool is copyleft except butler:**
- Legendary, Heroic, Mythic, gogdl, nile = **GPL-3.0**; DepotDownloader = **GPL-2.0**.
- Static-linking / embedding GPL into our closed commercial app would force source disclosure.
- **Mitigation (per report):** talk to these tools as a **separate process over CLI/RPC**, not
  a linked library. That keeps GPL boundary clean (arm's-length aggregation). This must be a
  hard architectural rule for the store layer: spawn `legendary`/`gogdl` as child processes,
  parse stdout/JSON — never link.
- butler (MIT) = the only one freely embeddable.
- SteamCMD = Valve proprietary → using it inside a commercial product is legally unclear;
  DepotDownloader (GPL-2.0) same copyleft problem.

## Strategic read for MacRunner
1. **Launch-store priority = GOG + Epic.** Both fit native-download → our-Wine cleanly, no DRM
   client. GOG is the lowest-friction first storefront (DRM-free, gogdl, direct launch).
2. **Steam is the hard one** and it's exactly the inverse of Epic: with Epic the *client* under
   Wine is broken so you go native; with Steam the *client* under Wine WORKS and is basically
   required, so you run it under Wine. Our value-add over CrossOver/Whisky here is that Windows
   Steam runs on OUR pure-ARM64 Wine + HyperBridge (no Rosetta) instead of GPTK+Rosetta.
3. **Architecture rule:** store tools = child processes (GPL firewall). Only butler may be linked.
4. **Open items:** maintain our own DRM-free-Steam allowlist (Valve doesn't publish one); legal
   review of SteamCMD/DepotDownloader ToS before shipping any native-Steam-download feature.

---

## ORIGINAL REPORT (verbatim, citations preserved)

(See ChatGPT source; archived here for the record. Synthesis above is the actionable layer.)
