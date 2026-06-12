#!/usr/bin/env bash

# Lane A disjoint audio+input smoke runner.
# Focus: native ARM64 path with explicit x64 rows (BLOCKED by default).

set -euo pipefail

ROOT="${MACRUNNER_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
DIST_ARM64_EC="${MACRUNNER_WINE_DIST_ARM64:-$ROOT/engine/wine/dist-arm64ec-spike}"
DIST_X64="${MACRUNNER_WINE_DIST_X64:-$DIST_ARM64_EC}"
PREFIX="${MACRUNNER_AUDIO_INPUT_PREFIX:-$ROOT/artifacts/audio-input-smoke-prefix}"
TIMEOUT="${MACRUNNER_AUDIO_INPUT_TIMEOUT:-20}"
RUN_DIR="${MACRUNNER_AUDIO_INPUT_RUN_DIR:-$ROOT/reports/audio-input-smoke/$(date +%Y%m%d-%H%M%S)}"
SUMMARY_JSON="${MACRUNNER_AUDIO_INPUT_SUMMARY:-$RUN_DIR/summary.json}"
RUN_X64=0
FORCE_X64=0
INCLUDE_CASES=()
WINE_EXE_ARM64="${MACRUNNER_WINE_EXE_ARM64:-$DIST_ARM64_EC/bin/wine}"
WINE_EXE_X64="${MACRUNNER_WINE_EXE_X64:-$DIST_X64/bin/wine}"

usage() {
  cat <<'USAGE'
Usage:
  ./scripts/run-audio-input-native-smoke.sh [--timeout SECS] [--run-dir PATH]
    [--x64] [--force-x64] [--case-id ID] [--json PATH]

  --timeout    Timeout for each case (seconds). Default: 20.
  --run-dir    Output folder. Default: reports/audio-input-smoke/YYYYMMDD-HHMMSS.
  --x64        Include x64 rows (default BLOCKED by Lane A reasons).
  --force-x64  Run x64 rows (use only after x64 blockers are cleared).
  --case-id    Run only this case id (can be repeated).
  --json       Override summary path.
USAGE
}

while [ "${1:-}" != "" ]; do
  case "$1" in
    --timeout) TIMEOUT="${2:?--timeout requires seconds}"; shift 2 ;;
    --run-dir) RUN_DIR="${2:?--run-dir requires path}"; SUMMARY_JSON="${RUN_DIR}/summary.json"; shift 2 ;;
    --x64) RUN_X64=1; shift ;;
    --force-x64) RUN_X64=1; FORCE_X64=1; shift ;;
    --case-id) INCLUDE_CASES+=("${2:?--case-id requires id}"); shift 2 ;;
    --json) SUMMARY_JSON="${2:?--json requires path}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage; exit 2 ;;
  esac
done

mkdir -p "$RUN_DIR"/{cases,case-assets,logs} "$PREFIX"

should_run_case() {
  if [ "${#INCLUDE_CASES[@]}" -eq 0 ]; then
    return 0
  fi
  local id="$1"
  local candidate
  for candidate in "${INCLUDE_CASES[@]}"; do
    if [ "$candidate" = "$id" ]; then
      return 0
    fi
  done
  return 1
}

write_case() {
  local output="$1" id="$2" phase="$3" api="$4" arch="$5" exe="$6"
  local status_hint="$7" reason="$8" wrapper_rc="$9" runner_json="${10}"
  python3 - "$output" "$id" "$phase" "$api" "$arch" "$exe" "$status_hint" "$reason" "$wrapper_rc" "$runner_json" <<'PY'
import json
import os
import re
from pathlib import Path
import sys

output, cid, phase, api, arch, exe, status_hint, reason, wrapper_rc, runner_json = sys.argv[1:11]

payload = {
    "schema_version": 1,
    "id": cid,
    "phase": phase,
    "api": api,
    "arch": arch,
    "exe": exe,
    "status": status_hint,
    "pass": False,
    "rc": 0,
    "duration_ms": 0,
    "notes": reason or "",
    "stdout_path": "",
    "stderr_path": "",
    "stdout_tail": "",
    "wrapper_rc": int(wrapper_rc),
    "runner_payload": {},
    "evidence": {},
}

runner_data = {}
if status_hint == "AUTO":
    if runner_json and os.path.isfile(runner_json):
        try:
            runner_data = json.loads(Path(runner_json).read_text(encoding="utf-8"))
        except Exception as exc:
            runner_data = {}
            payload["notes"] = f"failed to parse runner json: {exc}"
            payload["status"] = "NO_JSON"
    else:
        payload["status"] = "NO_JSON"
        payload["notes"] = reason or "runner did not produce json"

if status_hint != "AUTO":
    payload["status"] = status_hint

if runner_data:
    status = runner_data.get("status", "NO_JSON")
    payload["status"] = status
    payload["rc"] = int(runner_data.get("rc", runner_data.get("exit_code", 0)))
    payload["duration_ms"] = int(runner_data.get("duration_ms", 0))
    payload["stdout_path"] = runner_data.get("stdout_path", "")
    payload["stderr_path"] = runner_data.get("stderr_path", "")
    payload["stdout_tail"] = (runner_data.get("stdout", "") or "")[-2048:]
    payload["runner_payload"] = runner_data
    payload["pass"] = status == "PASS"
    payload["notes"] = payload["notes"] or str(runner_data.get("error", "")).strip()

stdout = (runner_data.get("stdout") or "").lower()
if "input" in api.lower():
    hits = re.findall(r"\b(event|button|axis|trigger|key|mouse|joystick|hid)\b", stdout)
    if hits:
        payload["evidence"]["input_event_hits"] = len(hits)
if "xinput" in api.lower() or "dinput" in api.lower():
    payload["evidence"]["input_probe_output"] = (runner_data.get("stdout") or "").strip()[:300]
if any(token in api.lower() for token in ("winmm", "mmdevapi", "dsound", "xaudio", "audio")):
    match = re.search(r"latenc\w*[:=]\s*([0-9]+(?:\.[0-9]+)?)\s*ms", stdout)
    if match:
        try:
            payload["evidence"]["latency_ms"] = float(match.group(1))
        except ValueError:
            pass
    if "checksum" in stdout:
        payload["evidence"]["checksum_seen"] = True
    if "spectrum" in stdout:
        payload["evidence"]["spectrum_seen"] = True
    match = re.search(r"checksum=0x([0-9a-fA-F]{8})", stdout)
    if match:
        payload["evidence"]["checksum"] = match.group(1)
    match = re.search(r"NEGOTIATED format=.*", stdout)
    if match:
        payload["evidence"]["format"] = match.group(0)
    match = re.search(r"latency.*=\s*([0-9]+(?:\.[0-9]+)?)\s*ms", stdout)
    if match:
        payload["evidence"]["latency"] = match.group(1).strip()
if Path(exe).name in ("audio_device_enum_arm64.exe", "audio_device_enum_x64.exe"):
    payload["evidence"]["audio_probe_output"] = (runner_data.get("stdout") or "").strip()[:300]

Path(output).write_text(json.dumps(payload, sort_keys=True, indent=2) + "\n", encoding="utf-8")
PY
}

run_case() {
  local id="$1" phase="$2" api="$3" exe="$4" arch="$5" wine_exe="${6:-}"
  local case_dir="$RUN_DIR/cases/$id"
  local json_path="$case_dir/result.json"
  local log_path="$case_dir/run.log"
  local wrapper_json="$case_dir/wrapper.json"
  local stderr_path="$case_dir/stderr.log"
  local used_wine="$wine_exe"

  mkdir -p "$case_dir"
  should_run_case "$id" || return 0

  if [ -z "$used_wine" ]; then
    if [ "$arch" = "x64" ]; then
      used_wine="$WINE_EXE_X64"
    else
      used_wine="$WINE_EXE_ARM64"
    fi
  fi

  if [ ! -x "$exe" ]; then
    write_case "$json_path" "$id" "$phase" "$api" "$arch" "$exe" "GAP" "missing executable" 127 "$json_path"
    return 0
  fi

  if [ ! -x "$used_wine" ]; then
    write_case "$json_path" "$id" "$phase" "$api" "$arch" "$exe" "BLOCKED" "missing wine binary: $used_wine" 127 "$json_path"
    return 0
  fi

  if [ ! -d "$PREFIX" ]; then
    mkdir -p "$PREFIX"
  fi

  set +e
  MACRUNNER_AUDIO_INPUT_PREFIX="$PREFIX" WINEPREFIX="$PREFIX" python3 - "$exe" "$case_dir" "$TIMEOUT" "$used_wine" "$log_path" "$stderr_path" <<'PY'
import os
import subprocess
import time
import json
import pathlib
import sys
import traceback

exe, case_dir, timeout_s, wine_bin, stdout_path, stderr_path = sys.argv[1:7]
timeout = float(timeout_s)
start_ns = time.monotonic_ns()
payload = {
    "schema_version": 1,
    "status": "FAIL",
    "exit_code": 1,
    "rc": 1,
    "duration_ms": 0,
    "timeout_sec": int(timeout),
    "timed_out": False,
    "timeout": False,
    "exe": exe,
    "exe_path": exe,
    "workdir": case_dir,
    "command": [wine_bin, exe],
    "args": [],
    "env_overrides": [],
    "stdout_path": stdout_path,
    "stderr_path": "",
    "stdout": "",
    "stderr": "",
    "error": "",
}

env = os.environ.copy()
env["WINEPREFIX"] = os.environ.get("MACRUNNER_AUDIO_INPUT_PREFIX", env.get("WINEPREFIX", case_dir))
env["WINEDEBUG"] = os.environ.get("WINEDEBUG", "-all")
env["MACRUNNER_WINE_DIST"] = os.path.dirname(os.path.dirname(wine_bin))
env["MACRUNNER_WINE_DIST_ARM64"] = env.get("MACRUNNER_WINE_DIST_ARM64", os.path.dirname(os.path.dirname(wine_bin)))
env["MACRUNNER_AUDIO_BACKEND"] = "avaudio"
env["MACRUNNER_HB_X64_LOADER"] = "1"
env["MACRUNNER_AUDIO_TEST_OUT"] = case_dir

try:
    with open(stdout_path, "wb") as out_fh, open(stderr_path, "wb") as err_fh:
      result = subprocess.run(
        [wine_bin, exe],
        env=env,
        cwd=case_dir,
        timeout=timeout,
        stdout=out_fh,
        stderr=err_fh,
        check=False,
      )
    rc = result.returncode
    stdout = b""
    stderr = b""
except subprocess.TimeoutExpired as exc:
    rc = 124
    stdout = b""
    stderr = (exc.stderr or b"") + b"[runner] timed out\n"
    try:
      if exc.stdout:
        with open(stdout_path, "ab") as out_fh:
          out_fh.write(exc.stdout)
    except Exception:
      pass
except Exception as exc:
    rc = 125
    stdout = b""
    stderr = (traceback.format_exc()).encode("utf-8", errors="ignore")
    with open(stderr_path, "ab") as err_fh:
      err_fh.write(stderr)

with open(stderr_path, "ab") as err_fh:
    if stderr:
        err_fh.write(stderr)

stdout = b""
stderr = b""
try:
    stdout = pathlib.Path(stdout_path).read_bytes()
except Exception:
    pass
try:
    stderr = pathlib.Path(stderr_path).read_bytes()
except Exception:
    pass

duration_ms = int((time.monotonic_ns() - start_ns) / 1_000_000)
payload["duration_ms"] = duration_ms
payload["exit_code"] = int(rc)
payload["rc"] = int(rc)
payload["stdout"] = stdout.decode("utf-8", errors="replace")
payload["stderr"] = stderr.decode("utf-8", errors="replace")
payload["stderr_path"] = stderr_path
payload["timed_out"] = rc == 124
payload["timeout"] = rc == 124
if rc == 0:
    payload["status"] = "PASS"
elif rc == 124:
    payload["status"] = "TIMEOUT"
else:
    payload["status"] = "FAIL"

wrapper_payload = pathlib.Path(__import__("os").path.join(case_dir, "wrapper.json"))
wrapper_payload.write_text(json.dumps(payload, sort_keys=True, indent=2), encoding="utf-8")
pathlib.Path(__import__("os").path.join(case_dir, "status.txt")).write_text(payload["status"] + "\n", encoding="utf-8")
PY
  local wrapper_rc=$?
  set -e
  if [ "$wrapper_rc" -ne 0 ]; then
    write_case "$json_path" "$id" "$phase" "$api" "$arch" "$exe" "NO_JSON" "failed to execute wine runner" "$wrapper_rc" "$json_path"
    return 0
  fi

  if [ -f "$case_dir/wrapper.json" ] && [ -s "$case_dir/wrapper.json" ]; then
    cp "$case_dir/wrapper.json" "$json_path"
    write_case "$json_path" "$id" "$phase" "$api" "$arch" "$exe" "AUTO" "" "$wrapper_rc" "$json_path"
  else
    write_case "$json_path" "$id" "$phase" "$api" "$arch" "$exe" "NO_JSON" "runner did not write json" "$wrapper_rc" "$json_path"
  fi
}

emit_blocked_case() {
  local id="$1" phase="$2" api="$3" arch="$4" exe="$5" reason="$6"
  should_run_case "$id" || return 0

  local case_dir="$RUN_DIR/cases/$id"
  mkdir -p "$case_dir"
  write_case "$case_dir/result.json" "$id" "$phase" "$api" "$arch" "$exe" "BLOCKED" "$reason" 0 "$case_dir/result.json"
}

emit_arm64_cases() {
  run_case "winmm-tone-play" "phase-1" "WinMM" "$ROOT/fixtures/arm64/tone_winmm_arm64.exe" "arm64"

  run_case "mmdevapi-enum-render" "phase-1" "MMDevice/WASAPI" "$ROOT/fixtures/arm64/render_mmdevapi_arm64.exe" "arm64"

  run_case "dsound-buffer-mix" "phase-2" "DirectSound" "$ROOT/fixtures/arm64/mix_dsound_arm64.exe" "arm64"
  run_case "xaudio2-source-voice" "phase-2" "XAudio2" "$ROOT/fixtures/arm64/voice_xaudio2_arm64.exe" "arm64"
  run_case "dinput-enum-poll" "phase-3" "DInput/DInput8" "$ROOT/fixtures/arm64/poll_dinput_arm64.exe" "arm64"
  run_case "xinput-probe" "phase-3" "XInput" "$ROOT/fixtures/arm64/poll_xinput_arm64.exe" "arm64"
}

emit_x64_cases() {
  if [ "$RUN_X64" -ne 1 ]; then
    return 0
  fi

  local reason="BLOCKED(needs Lane A): overflow + ucrtbase native-entry allowlist"
  if [ "$FORCE_X64" != "1" ]; then
    emit_blocked_case "x64-winmm-tone-play" "phase-1" "WinMM" "x64" "$ROOT/artifacts/audio-test/sine-440-x64/sine_440.exe" "$reason"
    emit_blocked_case "x64-mmdevapi-enum-render" "phase-1" "MMDevice/WASAPI" "x64" "$ROOT/fixtures/x64/audio_device_enum_x64.exe" "$reason"
    emit_blocked_case "x64-dsound-buffer-mix" "phase-2" "DirectSound" "x64" "$ROOT/fixtures/x64/dsound_buffer_mix_x64.exe" "$reason"
    emit_blocked_case "x64-xaudio2-source-voice" "phase-2" "XAudio2" "x64" "$ROOT/fixtures/x64/xaudio2_source_voice_x64.exe" "$reason"
    emit_blocked_case "x64-dinput-enum-poll" "phase-3" "DInput/DInput8" "x64" "$ROOT/fixtures/x64/raw_input_register_x64.exe" "$reason"
    emit_blocked_case "x64-xinput-probe" "phase-3" "XInput" "x64" "$ROOT/fixtures/x64/xinput_probe_x64.exe" "$reason"
    return
  fi

  run_case "x64-winmm-tone-play" "phase-1" "WinMM" "$ROOT/artifacts/audio-test/sine-440-x64/sine_440.exe" "x64"
  if [ -x "$ROOT/fixtures/x64/audio_device_enum_x64.exe" ]; then
    run_case "x64-mmdevapi-enum-render" "phase-1" "MMDevice/WASAPI" "$ROOT/fixtures/x64/audio_device_enum_x64.exe" "x64"
  else
    emit_blocked_case "x64-mmdevapi-enum-render" "phase-1" "MMDevice/WASAPI" "x64" "$ROOT/fixtures/x64/audio_device_enum_x64.exe" "missing executable"
  fi
  run_case "x64-dsound-buffer-mix" "phase-2" "DirectSound" "$ROOT/fixtures/x64/dsound_buffer_mix_x64.exe" "x64" "$WINE_EXE_X64"
  run_case "x64-xaudio2-source-voice" "phase-2" "XAudio2" "$ROOT/fixtures/x64/xaudio2_source_voice_x64.exe" "x64" "$WINE_EXE_X64"
  run_case "x64-dinput-enum-poll" "phase-3" "DInput/DInput8" "$ROOT/fixtures/x64/raw_input_register_x64.exe" "x64"
  run_case "x64-xinput-probe" "phase-3" "XInput" "$ROOT/fixtures/x64/xinput_probe_x64.exe" "x64"
}

emit_summary() {
  python3 - "$RUN_DIR" "$SUMMARY_JSON" "$TIMEOUT" <<'PY'
import json
from pathlib import Path
run_dir = Path(__import__("sys").argv[1])
summary_path = Path(__import__("sys").argv[2])
timeout = int(__import__("sys").argv[3])
cases = []
for path in sorted((run_dir / "cases").glob("**/result.json")):
    try:
        cases.append(json.loads(path.read_text(encoding="utf-8")))
    except Exception:
        continue
summary = {
    "schema_version": 1,
    "lane": "audio-input",
    "run_dir": str(run_dir),
    "dist": str(Path(__import__("os").environ.get("MACRUNNER_WINE_DIST_ARM64", ""))),
    "timeout_sec": timeout,
    "count": len(cases),
    "pass_count": sum(1 for case in cases if case.get("pass") is True),
    "blocked_count": sum(1 for case in cases if case.get("status") == "BLOCKED"),
    "gap_count": sum(1 for case in cases if case.get("status") == "GAP"),
    "missing_count": sum(1 for case in cases if case.get("status") == "MISSING"),
    "cases": cases,
}
summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
print(f"AUDIO_INPUT_SMOKE_SUMMARY {summary_path}")
PY
}

emit_arm64_cases
emit_x64_cases
emit_summary
