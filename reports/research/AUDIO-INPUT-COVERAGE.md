# Audio+Input Native+x64 Coverage Matrix

Scope: `engine/audio-tests` native ARM64 smoke fixtures (`tone_winmm`, `render_mmdevapi`, `mix_dsound`, `voice_xaudio2`, `poll_dinput`, `poll_xinput`) executed through `engine/wine/dist-arm64ec-spike`.

Last native matrix run:
- `scripts/run-audio-input-native-smoke.sh --run-dir reports/audio-input-smoke/20260613-native --timeout 30 --x64`
- native run-dir summary: `reports/audio-input-smoke/20260613-native/summary.json`

Generated 2026-06-13 while Lane A blockers are still active (`overflow + ucrtbase native-entry`).

## API → phase → native ARM64 / x64 state

| API / API-Group | Phase | Native ARM64 | x64 Guest | Evidence / gap |
|---|---|---|---|---|
| WinMM tone playback (`tone_winmm`) | phase-1 | PASS (`WINMM_TONE duration_ms=2000.000 checksum=0x60101517 bytes=176400`; raw `tone_winmm.raw`, size=176400, md5=a5e02accbcf7a8ccb247bc08fff6a4e4) | BLOCKED(needs Lane A): overflow + ucrtbase native-entry allowlist | `winmm-tone-play` case PASS + raw capture in `cases/winmm-tone-play` |
| MMDevice/WASAPI render (`render_mmdevapi`) | phase-1 | PASS (`format=0x1 channels=2 rate=44100 bits=16 block=4 avg=176400`; `default_hns=100000 min_hns=50000 buffer_frames=1323`; `latency_default_ms=10.000 latency_min_ms=5.000 latency_buffer_ms=30.000`; `checksum=0x90E805F8`; raw `render_mmdevapi.raw`, size=176400, md5=c04c00e28d30ab0e5ed9954c774a95aa) | BLOCKED(needs Lane A): overflow + ucrtbase native-entry allowlist | `mmdevapi-enum-render` negotiated format/period logged and hash-checked render buffer |
| DirectSound buffer mix (`mix_dsound`) | phase-2 | PASS (`format PCM channels=2 rate=44100 bits=16 frames=22050`, `checksum=0x62053448`; raw `mix_dsound.raw`, size=88200, md5=b0a3fa51bc49d37e9af6a515795f6e81) | BLOCKED(needs Lane A): overflow + ucrtbase native-entry allowlist | `dsound-buffer-mix` PASS + checksum from render mix buffer |
| XAudio2 source voice (`voice_xaudio2`) | phase-2 | PASS (`format PCM channels=2 rate=44100 bits=16 frames=44100`, `checksum=0x423C8403`; raw `voice_xaudio2.raw`, size=176400, md5=ddd5dd9d1010fa5d12a37098ced42ca2) | BLOCKED(needs Lane A): overflow + ucrtbase native-entry allowlist | `xaudio2-source-voice` PASS + source-voice render probe |
| DInput/DInput8 enumerate+poll (`poll_dinput`) | phase-3 | PASS (`DINPUT_ENUM count=0 no-gamepad`, `DINPUT_POLL_OK`) | BLOCKED(needs Lane A): overflow + ucrtbase native-entry allowlist | `dinput-enum-poll` reports no attached gamepad + poll loop completes |
| XInput enumerate+poll (`poll_xinput`) | phase-3 | PASS (`XINPUT_ENUM count=0 no-device`, `XINPUT_POLL_OK connects=0 events=0`) | BLOCKED(needs Lane A): overflow + ucrtbase native-entry allowlist | `xinput-probe` reports no attached controller + poll loop completes |

## x64 switch rule

`--x64` in `scripts/run-audio-input-native-smoke.sh` emits x64 rows as `BLOCKED(needs Lane A)` by default.
Real x64 execution requires `--force-x64` **after** Lane A clears the blockers.

## x64 forced diagnostic (pilot) snapshot

Run:

- `scripts/run-audio-input-native-smoke.sh --run-dir reports/audio-input-smoke/20260613-native-force-x64 --timeout 30 --force-x64`

Observed in this pilot:
- `x64-dinput-enum-poll` → `PASS`, `input ok` (no attached pad)
- `xinput-probe` → `PASS`, `xinput probe ok` (no attached pad/events)
- `x64-winmm-tone-play` → `TIMEOUT` (30s cap)
- `x64-mmdevapi-enum-render` → `TIMEOUT` (30s cap)
- `x64-dsound-buffer-mix` → was `GAP` (`missing executable`) in this pilot
- `x64-xaudio2-source-voice` → was `GAP` (`missing executable`) in this pilot

This is not yet an acceptance matrix run; matrix remains blocked-by-Lane-A above.

## x64 fixture prep

Prepared 2026-06-13:
- `fixtures/x64/dsound_buffer_mix_x64.exe` built from `engine/audio-tests/mix_dsound.c`
- `fixtures/x64/xaudio2_source_voice_x64.exe` built from `engine/audio-tests/voice_xaudio2.c`
- `engine/audio-tests/build-audio-tests.sh` now regenerates both files

Next x64 acceptance after Lane A unblocks should no longer report DSound/XAudio2 fixture `GAP`.

## Target outcome (phase goal)

Tone path is now observed from a real ARM64 PE through native CoreAudio-to-WASAPI with measured default/min/driver buffer latency and negotiated PCM format, while both input paths enumerate and poll to stable zero-device/no-event output on this host.
