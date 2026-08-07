# AUDIO-FMOD-READINESS-AUDIT

Scope:
- Target: Unity 6000 + FMOD stack on Windows audio path.
- Goal date: 2026-06-13.
- Objective: map FMOD audio startup/output path and cross-check with current MacRunner audio smoke coverage before HK first sound.

## 1) Through which Windows audio API does FMOD go by default?

- FMOD default output is `FMOD_OUTPUTTYPE_AUTODETECT` and is described as choosing the best platform mode by default.
- For Windows, enum defaults indicate:
  - `FMOD_OUTPUTTYPE_WASAPI` — default on Windows Vista and above.
  - `FMOD_OUTPUTTYPE_DSOUND` — default on Windows XP and below.
- `System::setOutput` is not mandatory for normal startup; it is only used to override the OS-optimal default.
- Therefore, for HK6000 on modern Windows the default runtime path is:  
  `FMOD_OUTPUTTYPE_AUTODETECT -> WASAPI / MMDevice`.

## 2) FMOD init + first-sound likely call sequence (Windows/WASAPI path)

Known guaranteed FMOD calls from docs:
- `FMOD::System::create` → `FMOD::System::init(...)` initializes system object and opens sound device.
- `System::setOutput` is optional, only needed when forcing non-default backend.
- `System::setSoftwareFormat` should be left default unless game forces override.
- `System::setDSPBufferSize` controls mixer granularity/latency if set.

Likely lower-level stack for the default FMOD WASAPI path:
1. MMDevice enumeration/activation:
   - `CoCreateInstance(CLSID_MMDeviceEnumerator)` → `IMMDeviceEnumerator`
   - enumerate/select endpoint and activate `IAudioClient`.
2. Format negotiation and stream init:
   - `IAudioClient::GetMixFormat` (shared mode format preferred by engine).
   - `IAudioClient::Initialize` (called exactly once per interface; shared by default unless explicitly changed).
   - if event-driven path used: `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`, plus event handle setup.
3. Stream service and render loop:
   - `IAudioClient::GetService(IID_IAudioRenderClient)`.
   - `IAudioRenderClient::GetBuffer` + `ReleaseBuffer` pairs in the shared ring loop.
   - stream start (`IAudioClient::Start`) and periodic buffer progression.

Observed evidence from docs:
- `IAudioClient::Initialize` supports both shared/exclusive and event-driven flow.
- `Initialize` supports `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`.
- `IAudioClient::GetService` returns `IAudioRenderClient` for rendering.
- `IAudioRenderClient` requires strict Get/Release alternation and releases requested frame counts.

Fallback paths:
- `FMOD_OUTPUTTYPE_XAUDIO2` / XAudio2 API sequence (if FMOD forced): `XAudio2` creation + `CreateMasteringVoice`, `CreateSourceVoice`, `StartEngine/Start`, `SubmitSourceBuffer`-style flow (exact helper-level order is implementation-dependent in FMOD backend).
- `FMOD_OUTPUTTYPE_DSOUND` path is expected only on older OS profiles and lower compatibility mode.

## 3) Coverage cross-check vs current stack

### Current harness status matrix

| FMOD-expected API path | Native ARM64 status | x64 status | Evidence / numbers | Priority |
|---|---|---|---|---|
| **WASAPI (`MMDevice` + `IAudioClient`/`IAudioRenderClient`)** | **PASS** | **BLOCKED(needs Lane A)** | `MMDEVAPI_NEGOTIATED format=0x1 ch=2 rate=44100 bits=16`; `MMDEVAPI_PERIOD default_hns=100000 min_hns=50000 buffer_frames=1323`; `latency_default_ms=10.000 latency_min_ms=5.000 latency_buffer_ms=30.000`; `MMDEVAPI_CHECKSUM=0x90E805F8` (raw: `render_mmdevapi.raw`, 176400 bytes, md5 `c04c00e28d30ab0e5ed9954c774a95aa`). | P0 — primary expected HK path must be clean. |
| **XAudio2 (`IXAudio2Create`/`CreateMasteringVoice`/`CreateSourceVoice`)** | **PASS** | **BLOCKED(needs Lane A)** | `XAUDIO2 format=PCM ch=2 rate=44100 bits=16 frames=44100`; checksum `0x423C8403` (raw: `voice_xaudio2.raw`, 176400 bytes, md5 `ddd5dd9d1010fa5d12a37098ced42ca2`). | P1 — fallback for Unity/FMOD custom backend choices. |
| **DirectSound (`DirectSoundCreate`/`CreateSoundBuffer`)** | **PASS** | **BLOCKED(needs Lane A)** | `DSOUND format=PCM ch=2 rate=44100 bits=16 frames=22050`; checksum `0x62053448` (raw: `mix_dsound.raw`, 88200 bytes, md5 `b0a3fa51bc49d37e9af6a515795f6e81`). | P2 — should be covered only for forced DSOUND profiles. |
| **WinMM (`waveOut`)** | **PASS** | **BLOCKED(needs Lane A)** | `tone_winmm` checksum `0x60101517` (raw: `tone_winmm.raw`, 176400 bytes, md5 `a5e02accbcf7a8ccb247bc08fff6a4e4`). | P3 — legacy sanity gate. |
| **XInput/DInput input probes** | **PASS (both)** | **BLOCKED(needs Lane A)** | Pollers complete; no attached devices in current lab state; `XINPUT_POLL_OK`, `DINPUT_POLL_OK`. | P2 — gamepad path readiness for FMOD+input-rich titles. |

## 4) Readiness decision and action list

### Ready for HK audio on ARM64-native path?
Yes. The ready indicator for default FMOD/WASAPI path is now evidence-backed:
- format/latency negotiation observed;
- raw-file checksum validation in place;
- path execution is end-to-end through our native smoke fixtures.

### x64 acceptance gap and unblock condition
- All x64 rows remain `BLOCKED(needs Lane A)` with explicit reason:  
  `overflow + ucrtbase native-entry allowlist`.
- x64 acceptance fixtures for `DSound` and `XAudio2` are now prepared (`fixtures/x64/dsound_buffer_mix_x64.exe`, `fixtures/x64/xaudio2_source_voice_x64.exe`), so when Lane A unblocks no GAPs remain on fixture availability.
- On unblock: run acceptance one-shot in x64 mode and promote all rows to PASS if raw/audio checks still hold.

### Priority to close before HK launch
1. **P0**: Lane A unblock and rerun `--force-x64` harness for WASAPI/DSOUND/XAudio2/WinMM + input probes.
2. **P1**: If HK/Unity logs show explicit `setOutput` override, validate forced backends against dedicated fixture behavior (XAudio2/DSOUND).
3. **P2**: Keep loopback+checksum checks as mandatory evidence for any FMOD/Unity-specific tuning branch.

## 5) HK x64 Acceptance (`--force-x64`) — one-shot, no manual interpretation

### Canonical one-shot

Run exactly:
`scripts/run-audio-input-native-smoke.sh --run-dir reports/audio-input-smoke/<tag> --timeout 30 --x64 --force-x64`

Acceptance verdict is taken only from:
`reports/audio-input-smoke/<tag>/summary.json`

### Global PASS predicates for every x64 case

For each required case in the matrix:

- `case.status == "PASS"`
- `case.pass == true`
- `case.rc == 0` and `case.wrapper_rc == 0`
- `duration_ms > 0`
- no fallback evidence in logs:
  - `lower(case.stdout_tail)` must NOT contain `fallback`, `emulation`, `xaudio fallback`, `dsound fallback`, `wasapi fallback`, `software fallback`.
- failure-signature in logs absent:
  - no substring `: 0x` for paths that only print failures in success cases,
  - no substring ` failed` / `failed:` in case-specific API markers.

Audio-specific evidence fields are mandatory where available:

- `evidence.format` (e.g. for negotiated format)
- `evidence.checksum`
- `evidence.latency_ms` or `evidence.latency`

### API-by-API strict checks

#### A) WASAPI chain (`x64-mmdevapi-enum-render`, phase-1)

Expected call intent: FMOD_AUTODETECT default -> WASAPI (MMDevice→IAudioClient→IAudioRenderClient).

PASS criteria:

- `stdout` contains:
  - `MMDEVAPI_RENDER_OK`
  - `MMDEVAPI_NEGOTIATED format=...`
  - `MMDEVAPI_PERIOD default_hns=... min_hns=... buffer_frames=...`
  - `MMDEVAPI_CHECKSUM=0xXXXXXXXX`
- `evidence.format` includes format token and channel/sample-rate fields (channels/rate/bits).
- `evidence.latency_ms` present.
- `evidence.checksum` present and non-empty.
- No log fallback token from above.
- Raw file is present and non-empty (from `MMDEVAPI_CHECKSUM ... file=...`) and checksum validates.
- **hr=0 controls**:
  - if present, `stdout` may include `MMDEVAPI_FORMAT_SUPPORT preferred rc=...`; accepted return for this line is `0x00000000` or `0x00000001` (S_FALSE).
  - explicit failure lines (`CoCreateInstance ...: 0xXXXXXXXX`, `IMMDevice::Activate...`, `IAudioClient::Initialize`, `IAudioRenderClient::GetBuffer`, etc.) must be absent.

#### B) XAudio2 path (`x64-xaudio2-source-voice`, phase-2)

Expected call intent: `IXAudio2Create`/`CreateMasteringVoice`/`CreateSourceVoice`/`SubmitSourceBuffer`/`Start`.

PASS criteria:

- `stdout` contains:
  - `XAUDIO2_SOURCE_VOICE_OK`
  - `XAUDIO2 format=PCM ...`
  - `XAUDIO2 checksum=0xXXXXXXXX`
- `evidence.format` present (format token visible in stdout).
- `evidence.checksum` present and non-empty.
- No lines matching `XAUDIO2: .* failed:` in stdout.
- No fallback tokens in stdout.

#### C) DirectSound path (`x64-dsound-buffer-mix`, phase-2)

Expected call intent: `DirectSoundCreate8`/`SetCooperativeLevel`/`CreateSoundBuffer`/`Lock`/`Unlock`/`Play`.

PASS criteria:

- `stdout` contains:
  - `DSOUND_BUFFER_MIX_OK`
  - `DSOUND format=PCM ...`
  - `DSOUND checksum=0xXXXXXXXX`
- `evidence.format` present and `evidence.checksum` non-empty.
- No lines matching `DSOUND .* failed:`.
- No fallback tokens in stdout.

#### D) WinMM fallback sanity (`x64-winmm-tone-play`, phase-1 legacy)

PASS criteria:

- `stdout` contains:
  - `WINMM_TONE_OK`
  - `WINMM_TONE duration_ms=... checksum=0xXXXXXXXX ... bytes=...`
- no `winmm-waveout: .* failed` lines in stdout/stderr evidence.
- `evidence.checksum` present.

#### E) Input paths (`xinput`/`dinput`)

`x64-xinput-probe` and `x64-dinput-enum-poll` must not gate FMOD audio PASS, but must confirm input loop:

- `stdout` contains:
  - `XINPUT_POLL_OK` with `connects=`/`events=`
  - `DINPUT_POLL_OK` and `DINPUT_EVENTS polled=` (and optionally `DINPUT_ENUM count=0 no-gamepad`).
- `case.pass == true`, `rc == 0`.

### x64 acceptance outcome mapping

- If all required cases satisfy the above → `PASS` for HK x64 audio readiness.
- Any case violating criteria → `BLOCKED(needs Lane A)` until rerun after Lane A unblocks.
- For evidence artifacts, keep this one-shot command + summary json as the only gate.
