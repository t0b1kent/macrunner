# ChatGPT Pro — research briefs (2026-06-02)

For deep-research / o-series reasoning. ChatGPT has NO access to our repo and cannot run code —
these are purely EXTERNAL-knowledge tasks (public Unity / Mono / Wine / DXVK / DXMT / Metal docs,
bug trackers, source). Results feed Lane A's Hollow-Knight-to-window mission.

Current blocker context to give ChatGPT: we run an unmodified GOG **Hollow Knight.exe** (Unity,
Mono, x64 Windows build) on Apple Silicon via a custom x86→ARM64 translator + ARM64 Wine 11 +
ARM64EC + a D3D11→Metal layer (DXMT). The guest thread runs Mono managed-init but never reaches
`D3D11CreateDevice` inside our run window. The main thread is seen polling at a UnityPlayer
zero-timeout wait; workers sit on infinite waits. We must know whether this is a *gate* (waiting
on a handle/event that never signals) or *throughput* (init is just slow), and exactly what Unity
needs before it creates the graphics device.

---

## BRIEF 1 (PRIMARY) — Unity x64 player boot → graphics-device-create sequence + its synchronization

**Goal:** a precise, sourced map of what an unmodified Unity standalone player (Windows, Mono
scripting backend, roughly Unity 2017–2020 era — Hollow Knight territory) does from process start
until it creates the D3D11 device and opens its window, with emphasis on every place the MAIN
thread BLOCKS/WAITS and on what each wait depends on.

Answer specifically:
1. **Boot order:** UnityPlayer.dll entry → Mono runtime init (`mono_jit_init` / domain create /
   loading `Assembly-CSharp`, `mscorlib`, `UnityEngine.dll`) → engine subsystem init →
   GfxDevice/graphics-device selection → `D3D11CreateDevice` → swapchain/window. What is the real
   ordering — is the D3D11 device created on the MAIN thread or a dedicated render/gfx thread? When
   is the OS window (HWND) created relative to device create?
2. **The waits/gates:** during this boot, what OS synchronization does Unity/Mono use that the main
   thread blocks on — events, semaphores, condition vars, `WaitForSingleObject`/
   `WaitForMultipleObjects`, job-system worker handshakes, the Mono GC/finalizer thread, the Unity
   JobQueue/worker threads? Which of these MUST be signaled by another thread before the main thread
   proceeds to graphics init? Name the known handshake points (e.g. "main thread waits on worker
   pool ready", "render thread creates the device and signals main").
3. **Mono managed-init cost:** what dominates Mono startup time for a Unity game (JIT of how many
   methods, AOT vs JIT on Windows desktop, assembly loading, class init)? Rough method counts /
   what's the heaviest. Anything that makes Mono init pathologically slow under a *binary translator*
   (e.g. heavy use of a specific instruction class, exception-driven control flow, lots of small
   indirect calls, tight `bsearch`/sort loops in metadata)?
4. **Single vs multi-thread requirement:** can Unity's boot complete if worker threads are slow but
   present, or does it hard-require real concurrency / specific thread-local state? Does Mono boot
   spin a finalizer + GC thread that the main thread waits on?
5. **Render thread:** is Unity's "multithreaded rendering" on by default for standalone, and does
   that mean a separate thread owns D3D11 device creation? What's the main↔render thread signaling
   protocol at startup?

**Deliverable:** a step-by-step boot timeline with the wait/gate points called out, each tagged
[CONFIRMED] (with a source: Unity docs, Mono source, UnityPlayer reverse-engineering write-ups,
decompilations, forum/blog posts) or [INFERRED]. End with: "the 3 most likely single points where
a translator's main thread would park before D3D11CreateDevice, and what would un-block each."

---

## BRIEF 2 (SECONDARY) — what Unity needs from D3D11/DXGI for device-create to SUCCEED

**Goal:** the minimum D3D11/DXGI surface Unity requires so `D3D11CreateDevice` +
swapchain succeed, so our DXMT (D3D11→Metal) layer covers it.

Answer:
1. What `D3D11CreateDevice` parameters does Unity pass (feature levels requested — 11_0? 10_x
   fallback?, driver type, flags like BGRA_SUPPORT/SINGLETHREADED, the feature-level array)? What
   does Unity do on failure (fallback chain to WARP / lower feature level / D3D9)?
2. Which DXGI calls happen around it: `CreateDXGIFactory`, adapter enumeration
   (`EnumAdapters`/`EnumOutputs`), `CheckFeatureSupport`, `CheckFormatSupport` for which formats?
   Does Unity refuse to start if certain format/feature checks fail?
3. What swapchain does Unity create (`CreateSwapChain` vs `CreateSwapChainForHwnd`, flip-model?
   format, buffer count) and what must succeed for the first `Present`?
4. From the **DXVK** and **DXMT/D3D11-on-Metal** projects: what is the known-minimum set of D3D11
   features/format-support entries that real Unity games probe at startup, and which gaps have
   historically blocked Unity titles from creating a device? Any documented Unity-specific quirks in
   DXVK/vkd3d issue trackers.

**Deliverable:** a checklist "D3D11/DXGI entry points + feature/format-support flags Unity touches
before a window appears", each marked must-have vs optional, with sources (Unity manual/source,
DXVK/vkd3d/DXMT issues & code).

---

## BRIEF 3 (OPTIONAL) — Wine/Proton handling of Unity+Mono startup waits

**Goal:** what did Wine/Proton have to get right in the ntdll sync layer for Unity+Mono games to
boot, so we know which semantic our gate might be missing.

Answer: known Wine bugs/fixes around (a) `NtWaitForSingleObject`/`NtWaitForMultipleObjects`/
timers/`NtDelayExecution` for Unity, (b) Mono's threading (GC/finalizer, thread suspend for GC),
(c) Unity job-system worker waits, (d) zero-timeout polling loops that depend on another thread's
progress. Cite WineHQ AppDB (Hollow Knight + other Unity titles), Wine bugzilla, Proton issues.
**Deliverable:** list of concrete sync-semantics Wine implements that a half-wired emulator would
need, ranked by how likely each is to cause a main-thread park before graphics init.

---

### What NOT to ask ChatGPT
- Anything about OUR code/files (it can't see them).
- To run/build/test anything.
- x86→ARM64 instruction correctness (already solved here, 20M+ cases vs Unicorn).
Keep it to public, citable knowledge that informs WHERE to look in our own code.
