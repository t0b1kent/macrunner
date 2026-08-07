# Target Ladder (Tier 1/2/3) - Game Compatibility Queue

**Date:** 2026-06-06  
**Path:** [GEMINI-target-ladder.md](file:///Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/research/GEMINI-target-ladder.md)  

---

> [!IMPORTANT]
> This Target Ladder defines the game execution queue for MacRunner. It groups games by engine families to establish a clear pipeline from simple 2D platforms to complex 3D legacy architectures.
> We apply the **Note-117 KILL-FILTER** to immediately reject games with kernel-level or hypervisor-level anti-cheat (e.g., EAC, BattlEye, Vanguard) or intrusive third-party launcher overlays (e.g., Rockstar Social Club, EA App).
> Every target in the ladder is verified against public specifications and compatibility databases.

---

## 1. The Target Ladder Matrix

| Title & Sources | Engine + Version | Arch | Graphics API | DRM | Anti-Cheat | Store | Installer | SP/MP | Difficulty | Verdict & Rationale |
|---|---|---|---|---|---|---|---|---|---|---|
| **Hollow Knight**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Hollow_Knight)<br>[GOG Page](https://www.gog.com/game/hollow_knight)<br>[ProtonDB](https://www.protondb.com/app/367520) | Unity 2020.3 | x64 | D3D11 | DRM-free / Steam | None | GOG / Steam | Inno Setup | SP | 2 | **TIER 1 (In-Tree).** Standard Unity 2D/3D renderer. Excellent baseline for SEH/JIT thread validation. |
| **AI War 2**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/AI_War_2)<br>[GOG Page](https://www.gog.com/game/ai_war_2)<br>[ProtonDB](https://www.protondb.com/app/573410) | Unity 2021.3.45f2 | x64 | D3D11 | DRM-free | None | GOG | Inno Setup | SP/COOP | 2 | **TIER 1 (In-Tree).** GOG release is clean and has no dependencies on online stores. Tests complex UI rendering. |
| **Ori and the Blind Forest**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Ori_and_the_Blind_Forest)<br>[GOG Page](https://www.gog.com/game/ori_and_the_blind_forest_definitive_edition)<br>[ProtonDB](https://www.protondb.com/app/280500) | Unity 2017.2 | x64 | D3D11 | DRM-free / Steam | None | GOG / Steam | Inno Setup | SP | 2 | **TIER 1.** High-performance 2D/3D engine. Tests standard Unity resource management and audio stack. |
| **Cuphead**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Cuphead)<br>[GOG Page](https://www.gog.com/game/cuphead)<br>[ProtonDB](https://www.protondb.com/app/268910) | Unity 2018.4 | x64 | D3D11 | DRM-free / Steam | None | GOG / Steam | Inno Setup | SP/COOP | 2 | **TIER 1.** Sprite-intensive 2D game. Low CPU/GPU load, ideal for validating early presentation rules. |
| **Outer Wilds**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Outer_Wilds)<br>[GOG Page](https://www.gog.com/game/outer_wilds)<br>[ProtonDB](https://www.protondb.com/app/753640) | Unity 2019.4 | x64 | D3D11 | DRM-free / Steam | None | GOG / Steam | Inno Setup | SP | 2 | **TIER 1.** Physics-heavy 3D title. Standard Unity graphics pipeline, no invasive wrappers. |
| **Firewatch**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Firewatch)<br>[GOG Page](https://www.gog.com/game/firewatch)<br>[ProtonDB](https://www.protondb.com/app/383870) | Unity 5.3 | x64 | D3D11 | DRM-free / Steam | None | GOG / Steam | Inno Setup | SP | 2 | **TIER 1.** Simple Unity 3D exploration title. Ideal for initial 3D frame rendering checks. |
| **Hellblade: Senua's Sacrifice**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Hellblade:_Senua%27s_Sacrifice)<br>[GOG Page](https://www.gog.com/game/hellblade_senuas_sacrifice)<br>[ProtonDB](https://www.protondb.com/app/414340) | UE 4.22 | x64 | D3D11 / D3D12 | DRM-free | None | GOG / Steam | Inno Setup | SP | 3 | **TIER 1.** Unreal Engine 4 representative. Tests advanced audio (binaural) and UE4's D3D11 viewport. |
| **Stray**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Stray)<br>[ProtonDB](https://www.protondb.com/app/1332010) | UE 4.27 | x64 | D3D11 / D3D12 | Steam Wrapper | None | Steam | Steam Depot | SP | 3 | **TIER 1.** Modern UE4 title. Heavy use of DXGI swapchain and shader translation. |
| **Katana Zero**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Katana_Zero)<br>[GOG Page](https://www.gog.com/game/katana_zero)<br>[ProtonDB](https://www.protondb.com/app/460950) | GameMaker 2 | x64 | D3D11 | DRM-free / Steam | None | GOG / Steam | Inno Setup | SP | 1 | **TIER 1.** Super lightweight 2D GameMaker. Minimal DLL dependencies, excellent for checking early GDI window bounds. |
| **Brotato**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Brotato)<br>[ProtonDB](https://www.protondb.com/app/1942280) | Godot 3.x | x64 | D3D11 / OpenGL| Steam Wrapper | None | Steam | Steam Depot | SP | 1 | **TIER 1.** Lightweight Godot representative. Verifies Godot's display/window context creation. |
| **Half-Life**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Half-Life)<br>[ProtonDB](https://www.protondb.com/app/70) | GoldSrc (custom)| x86 | OpenGL | Steam Wrapper | None | Steam | Steam Depot | SP/MP | 2 | **TIER 2 (In-Tree).** Standard 32-bit legacy OpenGL title. Useful for checking the PE32/WOW64 runtime. |
| **GTA Vice City**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Grand_Theft_Auto:_Vice_City) | RenderWare | x86 | D3D8 | Retail / Steam | None | Steam / Retail| None | SP | 3 | **TIER 2 (In-Tree).** Legacy D3D8 RenderWare title. Requires the D3D8->D3D9->DXMT bridge. |
| **GTA San Andreas**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Grand_Theft_Auto:_San_Andreas) | RenderWare | x86 | D3D9 | Retail / Steam | None | Steam / Retail| None | SP | 3 | **TIER 2.** Classic D3D9 title. Tests RenderWare fixed-function shaders and legacy memory layout. |
| **Diablo 1 / Hellfire**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Diablo) | Custom | x86 | DirectDraw | DRM-free | None | GOG | Inno Setup | SP/COOP | 2 | **TIER 2 (In-Tree).** 8-bit DirectDraw title. Needs the DirectDraw palette oracle to render. |
| **Diablo 2 (2000)**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Diablo_II) | Custom | x86 | DDraw / Glide | Retail | None | Battle.net | Blizzard | SP/COOP | 3 | **TIER 2.** Classic 2D game. Great test for 32-bit assembly lifter and paletted frame rendering. |
| **Max Payne**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Max_Payne) | MaxFX | x86 | D3D8 | Steam Wrapper | None | Steam | Steam Depot | SP | 3 | **TIER 2.** Legacy D3D8 title. Checks fixed-function shader emulation and old audio libraries. |
| **Portal**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Portal) | Source | x86/x64 | D3D9 | Steam Wrapper | None | Steam | Steam Depot | SP | 2 | **TIER 2.** Valve Source engine representative. Uses standard D3D9 pipeline. |
| **Half-Life 2**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Half-Life_2) | Source | x86/x64 | D3D9 | Steam Wrapper | None | Steam | Steam Depot | SP | 2 | **TIER 2.** Tests Source engine's physics and material rendering. Highly compatible. |
| **Mount & Blade: Warband**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Mount_%26_Blade:_Warband) | Custom | x86 | D3D9 | Steam Wrapper | None | Steam/GOG | Steam/Inno | SP/MP | 2 | **TIER 2.** Custom DX9 engine. Low complexity but tests old DLL inputs. |
| **Limbo**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Limbo) | Custom | x86 | D3D9 | Steam Wrapper | None | Steam/GOG | Steam/Inno | SP | 2 | **TIER 2.** Playdead custom 2D engine. Verifies basic Direct3D 9 viewport presentation. |
| **Inside**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Inside) | Unity (custom) | x64 | D3D11 | Steam Wrapper | None | Steam/GOG | Steam/Inno | SP | 2 | **TIER 2.** Highly stylized 3D platformer. Tests modern Unity custom render passes. |
| **Celeste**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Celeste) | FNA / XNA | x64 | OpenGL / D3D11| Steam Wrapper | None | Steam/GOG | Steam/Inno | SP | 2 | **TIER 2.** C# FNA-based title. Tests .NET/CLR wrapper runtime and input management. |
| **Terraria**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Terraria) | FNA / XNA | x86 | D3D9 / D3D11 | DRM-free | None | GOG | Inno Setup | SP/MP | 2 | **TIER 2 (In-Tree).** FNA/XNA 32-bit title. Tests .NET Framework 4.0 runtime on WOW64. |
| **Jedi: Fallen Order**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Star_Wars_Jedi:_Fallen_Order) | UE 4.x | x64 | D3D11 / D3D12 | EA + Denuvo | Denuvo DRM | Steam/EA | EA App | SP | 5 | **REJECTED.** Contains Denuvo DRM and requires the EA App background launcher (anti-cheat/wrapper blocker). |
| **Apex Legends**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Apex_Legends) | Source (modded) | x64 | D3D11 | EA + Steam | EAC | Steam / EA | Steam/EA | MP | 5 | **REJECTED.** Kernel-level Easy Anti-Cheat (EAC) will not load or run under Wine/HyperBridge. |
| **Valorant**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Valorant) | UE 4.x | x64 | D3D11 | Riot Client | Vanguard | Riot | Riot Client| MP | 5 | **REJECTED.** Vanguard anti-cheat requires a kernel-level driver (`vgk.sys`) (architectural block). |
| **Destiny 2**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Destiny_2) | Tiger (custom) | x64 | D3D11 | Steam Wrapper | BattlEye | Steam | Steam Depot | MP | 5 | **REJECTED.** Intrusive anti-cheat (BattlEye) prevents execution under translation layers. |
| **Doom Eternal**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Doom_Eternal) | id Tech 7 | x64 | Vulkan | Denuvo | None | Steam / Bethesda| Steam/Bnet| SP/MP | 4 | **TIER 3.** Vulkan-only title. Avoids DXMT completely but requires robust Vulkan passthrough. |
| **Cyberpunk 2077**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Cyberpunk_2077) | REDengine 4 | x64 | D3D12 | DRM-free | None | GOG / Steam | Inno Setup | SP | 4 | **TIER 3.** D3D12-only, extremely heavy GPU workload. Deferred until D3D11 path is optimized. |
| **Elden Ring**<br>[PCGamingWiki](https://www.pcgamingwiki.com/wiki/Elden_Ring) | PhyreEngine | x64 | D3D12 | Steam Wrapper | EAC | Steam | Steam Depot | SP/MP | 5 | **REJECTED.** Relies on EAC and D3D12-only graphics (unless bypassed by modifications). |

---

## 2. Target Priorities & Tiering Strategy

1. **Tier 1 (Priority 1 - Unity/GameMaker/Godot x64 D3D11):** Our current focus. Unblocking Hollow Knight and AI War 2 immediately unblocks Ori, Cuphead, and Outer Wilds.
2. **Tier 2 (Priority 2 - Legacy Engines x86 D3D8/D3D9/DDraw):** Requires stabilizing the PE32 WOW64 CPU simulator (`wow64cpu`/`xtajit`). Once Terraria, Diablo 1, and GTA Vice City run, it proves legacy 32-bit games are product-ready.
3. **Tier 3 (Priority 3 - Advanced Engines x64 Vulkan/D3D12):** Cyberpunk 2077 and Doom Eternal. Deferred to later stages of development due to DX12/vkd3d and heavy GPU workloads.
