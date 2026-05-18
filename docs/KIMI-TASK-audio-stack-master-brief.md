# Kimi Master Brief — MacRunner Audio Stack (CoreAudio modernization)

**Target agent**: Kimi (Moonshot K2 / long-context model)
**Workstream**: Parallel to Codex Phase H engine work
**Duration estimate**: 1-2 weeks focused work
**Conflict risk with Codex**: LOW (separate Wine DLLs, separate build target)

---

## 1. Project context (read first)

**MacRunner** = Mac product to run Windows applications on Apple Silicon without Rosetta dependency. Architecture:

- **HyperBridge** = our own x86_64 → ARM64 dynamic translator
- **Wine 11** ARM64-native fork
- **Future**: DXMT graphics, AI auto-config

Repository root: `/Volumes/MacOS/MacRunner`

Required reading (in this order):
1. [AGENTS.md](../AGENTS.md) — paths, scripts, process hygiene, mandatory protocols
2. [docs/ENGINE-ARCHITECTURE.md](ENGINE-ARCHITECTURE.md) — full architecture
3. This brief

**Important paths**:
- Repo root: `/Volumes/MacOS/MacRunner`
- Wine source: `/Volumes/MacOS/MacRunner/engine/wine/`
- Wine builds: `/Volumes/MacOS/MacRunner/engine/wine/build-pure-arm64/`
- Wine dist (ARM64-native): `/Volumes/MacOS/MacRunner/engine/wine/dist-pure-arm64/`
- Your workspace (write only here for new work): `/Volumes/MacOS/MacRunner/engine/audio/macos/` (create)

**DO NOT touch** (Codex's active areas):
- `engine/hyperbridge/`
- `engine/wine/dlls/ntdll/`
- `engine/wine/dlls/kernelbase/`
- `engine/wine/dlls/win32u/`

---

## 2. Task

Implement modern, low-latency CoreAudio backend for Wine's audio stack on Apple Silicon.

### Current state

Wine has these audio components in `engine/wine/dlls/`:

| DLL | Purpose | Current state on macOS |
|---|---|---|
| `winecoreaudio.drv` | Native audio driver (HAL/AudioUnit) | Exists from upstream Wine; functional but old API |
| `mmdevapi` | Audio device enumeration (Windows IMMDeviceEnumerator) | Generic, calls driver |
| `dsound` | DirectSound (legacy audio API) | Forwards to driver |
| `xaudio2_0..xaudio2_9` | XAudio2 (modern audio API, games + many apps) | Forwards to driver |
| `x3daudio1_0..1_7` | X3DAudio (3D positional) | Wrappers |

**The problem**: existing `winecoreaudio.drv` uses old AudioUnit API (HAL Output Unit). It works but:
- Higher latency than necessary (5-15ms vs achievable 2-3ms)
- No spatial audio support
- No Apple Silicon-specific optimization (Audio Accelerator, M-series spatializer)
- Some users report glitches on Apple Silicon

**The goal**: implement parallel modern backend using `AVAudioEngine` (replacement for HAL Output Unit) as new driver option. Keep existing driver as fallback for compatibility.

### Scope (in priority order)

**Phase 1: Foundation (week 1)**
1. Study existing `winecoreaudio.drv/coreaudio.c` thoroughly — understand the unixlib interface Wine expects
2. Create new directory `engine/audio/macos/avaudio/` with:
   - `avaudio_driver.m` — Objective-C++ source implementing same unixlib interface as `coreaudio.c` but using AVAudioEngine
   - `Makefile.in` — Wine-compatible build
3. Implement basic playback: device init, stream open, render callback, stream close
4. Test with Windows-side test harness: simple beep through xaudio2_9 → mmdevapi → new driver

**Phase 2: Production-ready (week 2)**
5. Implement device enumeration (output + input)
6. Implement input/capture path (microphone)
7. Handle sample rate / format conversion via AVAudioConverter
8. Volume control via AVAudioMixerNode
9. Buffer size negotiation (target 256 samples / 5.3ms @ 48kHz)
10. Error handling, device disconnect/reconnect

**Phase 3 (stretch, only if Phase 1+2 solid)**:
11. Spatial audio (AVAudioEnvironmentNode) for X3DAudio integration
12. Hardware accelerator usage (AVAudioEngine on M-series)

### Out of scope (do NOT do)

- MIDI (coremidi.c stays as is for now)
- Bluetooth audio specifics (let macOS handle)
- Audio capture from system (security/privacy heavy work)
- Changes to xaudio2_*, dsound, mmdevapi DLLs (driver should be drop-in replacement)
- Any changes to HyperBridge or ntdll/kernelbase/win32u

---

## 3. Reference materials

### Wine side

- `engine/wine/dlls/winecoreaudio.drv/coreaudio.c` — **primary reference**, this is what you're modernizing. Read it fully (~2000 lines).
- `engine/wine/dlls/winecoreaudio.drv/coreaudio.h` — interface header
- `engine/wine/dlls/winecoreaudio.drv/unixlib.h` — the **contract** between Wine PE side and Unix driver side. Your new driver MUST implement same unixlib interface.
- `engine/wine/dlls/winealsa.drv/alsa.c` — ALSA reference for Linux, shows the same pattern with different backend

### macOS side

- AVAudioEngine: https://developer.apple.com/documentation/avfaudio/avaudioengine
- AVAudioSourceNode (custom render callback): https://developer.apple.com/documentation/avfaudio/avaudiosourcenode
- AVAudioFormat / AVAudioConverter for format conversion
- Note: AVAudioEngine wraps AudioUnit but with simpler lifecycle and better Apple Silicon perf

### Test apps for validation

After build, you can test with these (already in repo):
- `artifacts/phase-h/npp-x64/` — Notepad++ doesn't use audio, skip
- Need to find/extract a simple audio test exe (suggest: `WinUAE`, simple wav player, or write a minimal x64 PE that uses XAudio2)

---

## 4. Critical constraint: Wine unixlib pattern

Wine separates each driver into two halves:
- **PE side** (Windows DLL, runs as Windows code): `aarch64-windows/winecoreaudio.drv.dll`
- **Unix side** (native .so, runs as macOS code): `aarch64-unix/winecoreaudio.drv.so`

They communicate via **unixlib calls** — a strict ABI of function indices. Your new backend must implement EXACTLY the same unixlib functions (`get_endpoint_ids`, `create_stream`, etc.) so Wine's PE side doesn't need changes.

This means: **your new code lives in the Unix half ONLY**. Same .dll on Windows side, swappable .so on Unix side.

Build target: produce a `winecoreaudio_avaudio.drv.so` that can be installed alongside or replace existing `winecoreaudio.drv.so`.

---

## 5. Build & integration

### Build setup

Your new driver must build with Wine's makefile system. Pattern:

```makefile
# engine/audio/macos/avaudio/Makefile.in
MODULE    = winecoreaudio_avaudio.drv
UNIXLIB   = winecoreaudio_avaudio.drv.so
IMPORTS   = uuid

EXTRADLLFLAGS = -mcygwin

SOURCES = \
    avaudio_driver.m

unix_LDFLAGS = -framework AVFoundation -framework AudioToolbox -framework Foundation
```

Add to top-level Wine `configure.ac` and `Makefile.in` as new DLL. Or, simpler: create as standalone makefile that builds into Wine dist post-hoc.

**Recommended initial approach**: standalone Make target invoked by `scripts/build-audio-macos.sh` (you create), produces `.so` that gets installed into `engine/wine/dist-pure-arm64/lib/wine/aarch64-unix/`. Don't modify Wine's main Makefile.in yet — that's a merge conflict risk.

### Integration with existing winecoreaudio.drv

For Phase 1, **rename** approach is simplest:
- Keep existing `winecoreaudio.drv.so` as fallback
- Your driver builds as `winecoreaudio_avaudio.drv.so`
- Env switch: `MACRUNNER_AUDIO_BACKEND=avaudio` → loader picks your driver

For Phase 3, if successful, this can become default.

---

## 6. Methodology rules (from AGENTS.md)

These apply to you exactly as to Codex:

1. **Family audit before fixes** — if you encounter a Wine API gap, audit the whole family (e.g. if `get_endpoint_ids` broken, check `get_endpoint_format` too)
2. **Patch-by-evidence, never by suspicion** — don't add error paths "just in case"; only based on observed failures
3. **Test changes before claiming done** — minimum: build clean, install, run a Windows audio app, verify sound output

### Process hygiene

Same as Codex:
- Use `pkill -9 -f '[w]ineserver|[w]ine-preloader|...'` before/after Wine runs
- Test apps via `./scripts/run-notepad-x64.sh` pattern (create similar for audio test)
- Verify no Wine processes left after each run

---

## 7. Deliverables

### Phase 1 deliverable (week 1)

- [ ] `engine/audio/macos/avaudio/avaudio_driver.m` — minimum: device init, default output device, playback stream with render callback
- [ ] `engine/audio/macos/avaudio/Makefile.in` (or standalone build script)
- [ ] `scripts/build-audio-macos.sh` — wrapper to build + install
- [ ] `docs/AUDIO-STACK-AVAUDIO-DESIGN.md` — short design doc: 1 page on architecture, unixlib mapping, threading model
- [ ] Successful playback test: a windows audio test app produces sound through your driver

### Phase 2 deliverable (week 2)

- [ ] Input/capture support
- [ ] Multiple device enumeration
- [ ] Format conversion (8/16/24/32-bit int, float; mono/stereo/5.1)
- [ ] Latency benchmark: round-trip < 10ms p99 on M1 Air
- [ ] `docs/AUDIO-STACK-VALIDATION.md` — test results, supported features, known limits

### Phase 3 (stretch) deliverable

- [ ] X3DAudio HRTF integration via AVAudioEnvironmentNode
- [ ] Spatial audio test (5.1 channel app through AirPods Pro)

---

## 8. Validation tests

Required tests for "done":

1. **Smoke**: Wine builtin `sndvol.exe` (if available) shows your driver as device
2. **Real app**: A small Windows audio test exe plays 440Hz sine wave through your driver — audibly correct, no glitches in 30 seconds
3. **Latency**: measure round-trip with stopwatch (input event → audio output) — target < 50ms (perception), ideal < 10ms
4. **Stability**: 5-minute continuous playback with no underruns logged
5. **Build**: clean from scratch with `./scripts/build-audio-macos.sh` succeeds on fresh checkout

---

## 9. Communication

Each work session, append to `docs/KIMI-PROGRESS-audio.md`:

```markdown
## YYYY-MM-DD

What was done today:
- [bullet points]

Blockers / questions:
- [bullet points]

Next session plan:
- [bullet points]
```

If you need to ask user-facing questions, file them in same doc under "Questions for Timur".

If your work overlaps unexpectedly with Codex's areas — STOP and add to questions. Don't modify Codex's files even if it seems necessary.

---

## 10. First-session goals

For your very first session, do exactly:

1. Read AGENTS.md and confirm you understand paths/protocols
2. Read existing `engine/wine/dlls/winecoreaudio.drv/coreaudio.c` fully
3. Read `engine/wine/dlls/winecoreaudio.drv/unixlib.h` and enumerate all unixlib functions you'll need to implement
4. Create `docs/AUDIO-STACK-AVAUDIO-DESIGN.md` skeleton with:
   - Unixlib function list (just names + signatures, no implementation yet)
   - Threading model proposal (which calls run on render thread vs main)
   - Initial concerns/open questions
5. Append to `docs/KIMI-PROGRESS-audio.md` with summary

**Do NOT write code yet** in session 1. Design first. Code in session 2.

---

## Appendix A — Quick path reference

```
/Volumes/MacOS/MacRunner                                # repo root
├── AGENTS.md                                            # READ FIRST
├── config/env.sh                                        # source for paths
├── engine/audio/macos/avaudio/                          # YOUR WORKSPACE (create)
├── engine/wine/dlls/winecoreaudio.drv/                  # REFERENCE (read, don't modify)
├── engine/wine/dlls/xaudio2_9/                          # REFERENCE
├── engine/wine/dist-pure-arm64/lib/wine/aarch64-unix/   # where built .so goes
└── docs/KIMI-PROGRESS-audio.md                          # YOUR PROGRESS LOG
```

## Appendix B — Sanity check first command

```bash
cd /Volumes/MacOS/MacRunner
. config/env.sh
echo "Root: $MACRUNNER_ROOT"
echo "Wine ARM64 dist: $MACRUNNER_WINE_DIST_ARM64"
ls engine/wine/dlls/winecoreaudio.drv/
```

If this works and shows files — you're in the right place.

---

End of brief. Welcome to MacRunner.
