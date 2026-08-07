# GEMINI MEGA-PROGRAM — Audio + Input lane (DSound/XAudio2/DInput/XInput → CoreAudio/GameController, arm64)

**Agent:** Gemini (agy) — proven systems coder. New, **disjoint** lane: game audio + input. No active
lane touches these DLLs (Lane A=ntdll/hb, Lane C=loader/server[PARKED], Lane D=graphics, PE32=xtajit).
**Horizon:** MONTHS. A PROGRAM, not a task. Phased, autonomous, evidence-driven.
**Date:** 2026-06-07
**Mission:** real Windows games on MacRunner get **working sound and working controllers/input** —
mixed 3D audio out through CoreAudio at low latency, and gamepad/keyboard/mouse/joystick in through
macOS GameController/IOKit — end-to-end, the way real titles use it. Today only isolated probes pass;
make the FULL path game-grade.

## WHERE WE START (verified — build on this, don't redo)
- DLLs already present in the active tree: `engine/wine/dlls/{winecoreaudio.drv, mmdevapi, winmm,
  dsound, xaudio2_7, xaudio2_8, xaudio2_9, dinput, dinput8, xinput1_1..1_4, xinput9_1_0,
  winexinput.sys}`.
- Isolated native probes already PASS: `reports/native-service-arm64-audio_device_enum.json`
  (device enum), `reports/native-service-x64-xinput_probe.json` (xinput). Prior Lane C isolated runs:
  `reports/lane-c-arm64-api-audio-isolated-*`, `reports/lane-c-raw-input-arm64-focused-*`,
  `reports/lane-c-arm64-api-input-dpi-isolated-*`. Read these (ctx) before starting — they say what
  already works.
- **The GAP:** none of this proves a real x64 game OUTPUTS audio (XAudio2/DSound mixing → CoreAudio)
  or READS a controller (XInput/DInput → GameController) end-to-end under HyperBridge. That's your job.

## ⚡ AUTONOMY — FIND AND FIX, CHAIN, DON'T STOP
Chain Phase 0→6. When a phase is green, immediately deepen the next. You OWN the audio/input DLLs and
the macOS backend bridge — you FIX at the root (no "report and wait"). The answer to "continue?" is YES.
Only the 3 STOP conditions at the end are valid reasons to yield. "Done" is asymptotic.

## 🟣 SURVIVAL DISCIPLINE
- ALWAYS `ctx_execute` (language=javascript) for logs/reports — NEVER raw cat/grep/tail on big files.
  `ctx_batch_execute` is broken here (spawn /bin/zsh ENOENT) — don't use it.
- Heartbeat EVERY step → `reports/research/LANE-AUDIO-PROGRESS.md` (`TIME · phase · action ·
  result(numbers) · next`). Resume anchor for fresh threads.

## SCOPE / OWNERSHIP (hard — other lanes are live)
- **YOURS (edit):** `engine/wine/dlls/winecoreaudio.drv/**`, `mmdevapi/**`, `winmm/**`, `dsound/**`,
  `xaudio2_7|8|9/**`, `dinput/**`, `dinput8/**`, `xinput1_1|1_2|1_3|1_4|9_1_0/**`, `winexinput.sys/**`,
  and any new macOS audio/input bridge you add under these or a clearly-named `engine/audio_input/**`.
  Lane C is PARKED and these audio/input DLLs are now THIS lane's.
- **NEVER touch:** `engine/wine/dlls/ntdll/**`, `macrunner_hb.c`, `signal_arm64.c` (Lane A); `xtajit`/
  `xtajit64` (PE32/Lane A); `engine/dxmt|graphics|vkd3d` (Lane D); `server/**`, `ntdll/unix/loader.c`
  (Lane C). If you need a change there → `reports/research/LANE-AUDIO-NEEDS.md` + keep working.
- Your own smoke prefix: `artifacts/audio-input-smoke-prefix/`. Scoped kill only
  (`WINEPREFIX=$PWD/artifacts/audio-input-smoke-prefix wineserver -k`), NEVER global pkill.
- Run via `MACRUNNER_RUN_DIR=$RUNDIR scripts/mr-run.sh …` so you get auto-triage. Commit named files only.

## PHASES (top-to-bottom; always have a next item; loop for months)
### Phase 0 — Baseline + smoke harness
Build a tiny x64 audio+input smoke you control: a PE that (a) opens an XAudio2/DSound voice and plays a
known tone, (b) reads XInput state in a loop. Capture proof: audio device callback actually fires /
samples reach CoreAudio (verify, don't assume); controller state read. Establish the real PASS/FAIL
baseline vs the isolated probes. Own the runner end-to-end in your smoke prefix.

### Phase 1 — Audio OUTPUT path solid (winecoreaudio.drv → CoreAudio, arm64)
mmdevapi/WASAPI + winecoreaudio.drv: device enum (already PASS) → open → format negotiation → render
client → real PCM reaches CoreAudio with correct rate/channels, low latency, no underruns. Verify with
a captured/looped-back tone (numbers: latency ms, no glitch over N seconds). Fix root issues under arm64.

### Phase 2 — DSound + XAudio2 (what games actually call)
DirectSound: secondary buffers, mixing, 3D positioning, looping. XAudio2: source/submix/mastering
voices, X3DAudio 3D, built-in FX (reverb/EQ), sample-rate conversion, callbacks. Drive real multi-voice
mixed output through to CoreAudio. These are the APIs Unity/Unreal/native titles use — cover them.

### Phase 3 — Input (XInput + DInput → GameController/IOKit)
XInput: enumerate controllers via macOS GameController framework (or IOKit/HID), map buttons/sticks/
triggers, **rumble/force-feedback**, hot-plug. DInput/DInput8: device enumeration, keyboard/mouse/
joystick state, effects, cooperative levels, raw input. Verify with a real controller (numbers: button/
axis events observed).

### Phase 4 — In-game integration (coordinate with Lane D when graphics lands)
Once a real game reaches a window (Lane D graphics), verify audio+input IN-GAME: sound plays in sync,
controller/keyboard moves the character. Until then, drive everything via your smoke apps — do NOT idle
waiting on graphics.

### Phase 5 — Latency / perf / stability
Audio latency budget + no underruns over minutes; input latency; hot-plug robustness; multi-threaded
audio callback correctness (games drive audio from their own thread). Long-run stable.

### Phase 6 — CI smoke gate
Wire an audio-out + input-read smoke as a permanent regression gate.

## LIVING OUTPUT
`reports/research/LANE-AUDIO-PROGRESS.md` (heartbeat/resume) + a coverage note of which audio/input
APIs are proven working (with evidence: latency numbers, captured samples, controller events).

## STOP conditions (ONLY these — else loop the backlog for MONTHS)
1. A real game plays mixed audio through CoreAudio AND responds to a controller, stable over minutes
   (with captured evidence) — plus the API coverage note is green.
2. Hard blocker needing another lane's files → `reports/research/LANE-AUDIO-NEEDS.md` + keep working
   other phases.
3. Operator decision on a real trade-off (e.g. "no force-feedback API parity on macOS — stub or map?").

End each run's final message: which APIs moved to proven (numbers), what you fixed at root, and
`LOOP-STATUS: CONTINUE <next>` (almost always) / `BLOCKED <why>` / `GOAL` (only STOP #1). Write the
LOOP-STATUS line at column 0 in LANE-AUDIO-PROGRESS.md.
