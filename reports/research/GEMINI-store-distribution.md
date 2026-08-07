# Store Distribution & Installer Mechanics (Lane G Feed)

**Date:** 2026-06-06  
**Path:** [GEMINI-store-distribution.md](file:///Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/research/GEMINI-store-distribution.md)  

---

> [!NOTE]
> This document maps the integration landscape of the major digital game storefronts (GOG, Steam, Epic Games, itch.io) 
> and the technical requirements to support third-party repack/installer formats.

---

## 1. Store Distribution & Integration Matrix

| Store | Native Integration Tool | Auth / Download Mechanism | DRM / Execution Model | License / GPL Firewall Rule |
|---|---|---|---|---|
| **GOG** | [gogdl](https://github.com/Heroic-Games-Launcher/heroic-gogdl) | OAuth + GOG API (Direct Windows depots) | **DRM-Free.** Launches directly via `.exe` under MacRunner's Wine. | **GPL-3.0.** Must execute as a separate child process (GPL Firewall). |
| **Epic Games** | [Legendary](https://github.com/legendary-gl/legendary) | OAuth + Epic API | **Launcher-Free.** Game `.exe` runs directly under Wine; EOS overlay optional. | **GPL-3.0.** Must execute as a separate child process (GPL Firewall). |
| **Steam** | [DepotDownloader](https://github.com/SteamRE/DepotDownloader) / `SteamCMD` | Steam account credentials + 2FA | **Steamworks DRM.** Requires running Windows Steam client inside the bottle for most titles. | **GPL-2.0** (DepotDownloader) / **Proprietary** (SteamCMD). Spawn as child process. |
| **itch.io** | [butler](https://github.com/itch-io/butler) | OAuth + itch API | **DRM-Free.** Game `.exe` launches directly. | **MIT.** Can be directly linked or embedded in MacRunner closed source. |

---

## 2. The GPL Firewall Architecture

To protect MacRunner's proprietary commercial closed-source status while using open-source downloaders:
- **Hard Constraint:** We cannot link against or bundle any GPL-licensed library/code inside our main binary.
- **Architecture:** We communicate with `gogdl`, `legendary`, `DepotDownloader`, and [nile](https://github.com/imLinguin/nile) exclusively via **CLI-spawning / RPC**.
- **Implementation:**
  - MacRunner spawns the helper utility as a child process.
  - Arguments are passed through standard command-line flags (e.g. `--json` outputs).
  - Progress and errors are parsed from the stdout stream JSON structures.
  - The GPL boundary is preserved cleanly under "arm's-length aggregation".

---

## 3. Installer & Repack Mechanics (Lane G Checklist)

Third-party game installers (including official GOG/Inno Setup installers and compact repacks like FitGirl or R.G. Mechanics) use high-performance compression tools. Running these 32-bit processes inside our WOW64/xtajit translation layer requires implementing the following checklist:

### A. Large Address Aware (LAA) 3GB Memory Space
- **Requirement:** Repack decompressors (such as `unarc.dll` or `arc.exe`) allocate massive scratchpad tables in virtual memory.
- **WOW64 Goal:** The translation layer must support mapping memory beyond the 2 GB limit, up to `0xC0000000` (3 GB) on 32-bit guest address spaces.
- **Verification:** Ensure `NtAllocateVirtualMemory` does not fail when requesting pages in the high 2GB-3GB range.

### B. Anonymous Pipe & IPC Handling
- **Requirement:** Inno Setup launches helpers (e.g., `arc.exe`) and reads output bytes from pipes to render the extraction progress bar.
- **WOW64 Goal:** `CreatePipe` must return valid handles, and `ReadFile` on the read end must block and wake up properly on guest JIT threads.

### C. System Redirection & DLL Hygiene
- **Requirement:** 32-bit installers look for system files in `C:\windows\system32`.
- **WOW64 Goal:** WOW64 directory redirection must direct them to `C:\windows\syswow64` to load the 32-bit built-in DLLs.

### D. File I/O Throughput
- **Requirement:** Unpacking writes thousands of small assets to disk.
- **WOW64 Goal:** Fast I/O paths must be enabled in `NTDLL` file operations to minimize translation layer overhead on disk writes.

---

## 4. References & Sources
- **GOGDL Repository:** [Heroic-Games-Launcher/heroic-gogdl (GitHub)](https://github.com/Heroic-Games-Launcher/heroic-gogdl)
- **Legendary Repository:** [legendary-gl/legendary (GitHub)](https://github.com/legendary-gl/legendary)
- **DepotDownloader Repository:** [SteamRE/DepotDownloader (GitHub)](https://github.com/SteamRE/DepotDownloader)
- **Nile Repository:** [imLinguin/nile (GitHub)](https://github.com/imLinguin/nile)
- **Butler Repository:** [itch-io/butler (GitHub)](https://github.com/itch-io/butler)

