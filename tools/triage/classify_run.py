import sys
import os
import subprocess
import json
import re

# Ensure tools/triage is in path so we can import triage_common
sys.path.append(os.path.dirname(os.path.abspath(__file__)))
from triage_common import find_logs, stream_events

def parse_analyzer_output(text):
    lines = text.splitlines()
    data = {
        "verdict": "UNKNOWN",
        "owner": "Unknown",
        "class": "UNKNOWN",
        "confidence": 0.0,
        "evidence": [],
        "next_action": "",
        "missing_markers": {"critical": [], "secondary": []},
        "next_run_env": {}
    }
    state = None
    sub_state = None
    for line in lines:
        line_strip = line.strip()
        if line.startswith("VERDICT:"):
            data["verdict"] = line.split(":", 1)[1].strip()
            state = None
        elif line.startswith("OWNER:"):
            data["owner"] = line.split(":", 1)[1].strip()
            state = None
        elif line.startswith("CLASS:"):
            data["class"] = line.split(":", 1)[1].strip()
            state = None
        elif line.startswith("CONFIDENCE:"):
            try:
                data["confidence"] = float(line.split(":", 1)[1].strip())
            except:
                data["confidence"] = 0.0
            state = None
        elif line.startswith("EVIDENCE:"):
            state = "evidence"
        elif line.startswith("NEXT_ACTION:"):
            state = "next_action"
        elif line.startswith("MISSING_MARKERS:"):
            state = "missing_markers"
            sub_state = None
        elif line.startswith("NEXT_RUN_ENV:"):
            state = "next_run_env"
        elif state == "evidence":
            if line.startswith("  "):
                data["evidence"].append(line_strip)
            elif line_strip == "":
                pass
            else:
                state = None
        elif state == "next_action":
            if line.startswith("  "):
                if data["next_action"]:
                    data["next_action"] += " " + line_strip
                else:
                    data["next_action"] = line_strip
            elif line_strip == "":
                pass
            else:
                state = None
        elif state == "missing_markers":
            if "CRITICAL:" in line:
                sub_state = "critical"
            elif "SECONDARY:" in line:
                sub_state = "secondary"
            elif line.startswith("    - "):
                marker = line.strip().strip("-").strip()
                if sub_state:
                    data["missing_markers"][sub_state].append(marker)
            elif line.startswith("  "):
                pass
            elif line_strip == "":
                pass
            else:
                state = None
        elif state == "next_run_env":
            if line.startswith("  "):
                if "=" in line_strip:
                    k, v = line_strip.split("=", 1)
                    data["next_run_env"][k.strip()] = v.strip()
            elif line_strip == "":
                pass
            else:
                state = None
    return data

ANALYZERS = [
    "analyze_dll_load.py",
    "analyze_waits.py",
    "analyze_window_gate.py",
    "analyze_arm64ec_callbacks.py",
    "analyze_smc.py",
    "analyze_d3d_gate.py",
    "analyze_pe32_wow64_handoff.py",
    "analyze_generic_fallback.py",
]

# --- Boot-ladder progression tracking (added 2026-06-10) -------------------
# Fixed HK-x64 boot ladder. Each run is scored by the FURTHEST rung whose
# marker appears in its logs; the best-ever rung is persisted next to the run
# dirs (<parent>/.triage-ladder-best.json). A run landing BELOW the best rung
# is flagged REGRESSION loudly — the "rabbit-hole detector" (the 6h 06-10
# thrash would have been flagged on the FIRST run: stuck before Ldr while
# best was GfxDevice).
LADDER_RUNGS = [
    ("after-loader",   ["phase=after-loader"]),
    ("xtajit64-init",  ["macrunner-xtajit64: ProcessInit status=00000000",
                        "loader ProcessInit/ThreadInit completed"]),
    ("ldr-entry",      ["ldr-entry"]),
    ("after-xlat",     ["after-xlat"]),
    ("loader-init",    ["after-loader-init", "loader_init phase"]),
    ("thread-start",   ["RtlUserThreadStart"]),
    ("mono-init",      ["Mono path[0]", "mono-2.0-bdwgc.dll"]),
    ("create-window",  ["NtUserCreateWindowEx", "create-window-handle",
                        "CreateWindowEx"]),
    ("gfxdevice",      ["GfxDevice: creating device", "GfxDevice client"]),
    ("dxgi-factory",   ["CreateDXGIFactory"]),
    ("d3d11-device",   ["D3D11CreateDevice hr=0", "D3D11CreateDevice succeeded"]),
    ("swapchain",      ["CreateSwapChain"]),
    # --- Pixel-gate rungs 12-14 (added 2026-07-04, lane pixelgate) ----------
    # Operator rule: a frame on rungs 12-14 is gated ONLY through
    # scripts/pixel-truth-gate.sh, never raw string-count markers. History:
    # the old soft "present" rung ("present_reached=YES"/"Present hr=0"/
    # "present_count=") and "window-visible" rung ("CG-capture"/
    # "non_background_pixels") ложняк'd on slot-8/MakeWindowAssociation false
    # Present and on "Present string count" heuristics. Replaced with the hard
    # trio below; rung 14 (real-present) is a CALL-level marker and the actual
    # PIXEL confirmation is the orthogonal PIXEL_MOMENT_REACHED flag
    # (rung==14 AND a pixel-truth-gate result.json artifact with verdict
    # NONBLACK/COLORFUL inside the run dir).
    #
    # DXMT source anchors (PRESENTPATH-ENGINE-HANDOFF.md):
    #   GetBuffer(0)              d3d11_swapchain.cpp:283
    #   CreateRenderTargetView    d3d11_texture_device.cpp:162
    #   Present / Present1        d3d11_swapchain.cpp:278 / :784
    # The HB swapchain vtable tracer (macrunner_hb.c:4623-4755) emits two
    # shapes:
    #   REAL  (known swapchain only, :4749):
    #     macrunner-hb-dxgi-swapchain: method=<m> slot=<n> swapchain=<ptr> rc=<ptr> ...
    #   CANDIDATE (unconfirmed, :4715, fires ONLY for slot 8/22):
    #     macrunner-hb-dxgi-swapchain: candidate method=<m> slot=<n> object=<ptr> rc=<ptr> ...
    # slot 8=Present, 9=GetBuffer, 13=ResizeBuffers, 22=Present1. The slot-8
    # "candidate" line is the MakeWindowAssociation ложняк (factory slot 8
    # misread as Present). Hard rule below: real-method line, no "candidate",
    # rc=0. GetBuffer (slot 9) has no candidate path at all -> inherently safe.
    ("getbuffer",      ["macrunner-hb-dxgi-swapchain: method=GetBuffer slot=9",
                        "dxmt-hk-swaptrace: kind=SwapChain method=GetBuffer"]),
    ("rtv",            ["macrunner-hb-d3d-rtv: CreateRenderTargetView rc=0x0",
                        "dxmt-hk-swaptrace: kind=Device method=CreateRenderTargetView"]),
    # The DXMT twin is NOT optional here, and its absence cost a day of wrong verdicts.
    # getbuffer and rtv above each carry both sides -- the HB vtable tracer and the DXMT one --
    # while this rung carried only HB, and only the exact spelling "Present slot=8". Hollow Knight
    # calls IDXGISwapChain1::Present1, so a run that cleared its render target, issued 39
    # DrawIndexed calls and presented was still reported as "rung 13, PIXEL_MOMENT_REACHED: no".
    # Every run in the archive is understated the same way.
    #
    # "method=Present" is left as a substring so it covers Present1 as well; inside a
    # kind=SwapChain line both spellings are the real presentation call, which is the thing this
    # rung is asking about.
    ("real-present",   ["macrunner-hb-dxgi-swapchain: method=Present slot=8",
                        "dxmt-hk-swaptrace: kind=SwapChain method=Present"]),
]

# IAT-binding diagnostic lines (dlls/ntdll/loader.c ~3332-3488: iatentry/cpdecision/
# cpresult/iatfinal all fire from the SAME import-table-patching routine and name
# imports at LOAD time — they are NOT real graphics calls. A bare-substring marker
# like "CreateDXGIFactory" would false-match any of them and report a graphics rung
# (dxgi-factory) the run never actually reached. Reject any hit on such a line.
_IATENTRY_TAGS = ("iatentry", "cpdecision", "cpresult", "iatfinal")

# HK rung12 false-positive: UnityPlayer+0x173fc30/0x173fca4 is FMOD thread
# stop/run-loop machinery, not a render gate. See:
# reports/phase4-hollow-knight/HK-RUNG12-CLEAR-GATE-FMOD-THREAD-REPORT.md
_NON_RENDER_GATE_TAGS = ("0x173fc30", "0x173fca4", "UnityPlayer+0x173fc30", "UnityPlayer+0x173fca4")

def _marker_hit(text, marker):
    """Substring hit, but reject (a) counter lines like 'D3D11CreateDevice=0' /
    'GfxDevice_count=...' (char right after the marker must not be = or _), and
    (b) IAT-binding diagnostic lines ('macrunner-hb-iatentry: ...!CreateDXGIFactory1',
    and its cpdecision/cpresult/iatfinal siblings) which name imports at load time,
    not real calls."""
    start = 0
    while True:
        i = text.find(marker, start)
        if i < 0:
            return False
        j = i + len(marker)
        if text[j:j + 1] not in ("=", "_"):
            line_start = text.rfind("\n", 0, i) + 1
            line_end = text.find("\n", j)
            line = text[line_start:line_end if line_end >= 0 else len(text)]
            if not any(tag in line for tag in _IATENTRY_TAGS) and not any(tag in line for tag in _NON_RENDER_GATE_TAGS):
                return True
        start = j

def _swapchain_real_call_hit(text, marker):
    """Hard matcher for the macrunner-hb-dxgi-swapchain vtable tracer rungs
    (getbuffer / real-present). A rung marker is the REAL-method trace line
    emitted ONLY on a known/registered swapchain (macrunner_hb.c:4749):
        macrunner-hb-dxgi-swapchain: method=<m> slot=<n> swapchain=<ptr> rc=<ptr> ...
    Reject:
      - 'candidate' lines (:4715) — the slot-8/MakeWindowAssociation ложняк,
        emitted for unconfirmed objects as 'candidate method=<m> ... object='.
        These are the historical false-Present source.
      - IAT-binding diagnostic lines (iatentry/cpdecision/cpresult/iatfinal) —
        load-time import patching, not runtime calls.
      - non-zero rc — a real call that returned an error did not produce the
        resource/frame and must not credit the rung.
    """
    start = 0
    while True:
        i = text.find(marker, start)
        if i < 0:
            return False
        line_start = text.rfind("\n", 0, i) + 1
        line_end = text.find("\n", i + len(marker))
        line = text[line_start:line_end if line_end >= 0 else len(text)]
        if ("candidate" not in line
                and not any(tag in line for tag in _IATENTRY_TAGS)
                and ("rc=0x0" in line or "rc=(nil)" in line or "rc=0x00000000" in line)):
            return True
        start = i + len(marker)

# Rung markers that must go through the hard swapchain matcher (not the soft
# _marker_hit substring matcher): the two dxgi-swapchain real-call rungs.
_SWAPCHAIN_HARD_MARKERS = (
    "macrunner-hb-dxgi-swapchain: method=GetBuffer slot=9",
    "macrunner-hb-dxgi-swapchain: method=Present slot=8",
)

def _rung_marker_hit(text, marker):
    if marker in _SWAPCHAIN_HARD_MARKERS:
        return _swapchain_real_call_hit(text, marker)
    return _marker_hit(text, marker)

def ladder_rung(text):
    """(idx, name) of the FURTHEST rung whose marker appears; (-1, None) if none.
    Rungs 12 (getbuffer) and 14 (real-present) use the hard swapchain matcher
    that rejects candidate/IAT/non-zero-rc lines, so the slot-8
    MakeWindowAssociation ложняк and 'Present string count' heuristics can no
    longer credit a pixel-gate rung."""
    best = (-1, None)
    for idx, (name, markers) in enumerate(LADDER_RUNGS):
        if any(_rung_marker_hit(text, m) for m in markers):
            best = (idx, name)
    return best

# --- Pixel-truth-gate artifact confirmation (orthogonal to the ladder) -----
# The ladder marks CALL-level progress (GetBuffer/RTV/real-Present reached).
# A real Present call does NOT prove a pixel was drawn — the first Present can
# blit an uninitialized (black) backbuffer. The operator rule: a FRAME on
# rungs 12-14 is confirmed ONLY through scripts/pixel-truth-gate.sh. Lanes
# invoke the gate with --rundir <run-dir>/pixel-truth-gate so the JSON
# artifact lands inside the run dir; this helper scans for it.
_PIXEL_VERDICTS_CONFIRMED = ("NONBLACK", "COLORFUL")

def pixel_truth_confirmed(run_dir):
    """Return (confirmed: bool, artifact_path: str|None, verdict: str|None).
    Scans <run_dir>/pixel-truth-gate/*/result.json for a NONBLACK/COLORFUL
    verdict. Also accepts a legacy single result.json at
    <run_dir>/pixel-truth-gate/result.json. Returns (False, None, None) when no
    artifact is present — the rung-14 call marker still stands, but
    PIXEL_MOMENT_REACHED stays False until a pixel-truth-gate artifact is
    captured."""
    import glob
    if not run_dir or not os.path.isdir(run_dir):
        return (False, None, None)
    base = os.path.join(run_dir, "pixel-truth-gate")
    candidates = sorted(glob.glob(os.path.join(base, "*", "result.json")))
    candidates.append(os.path.join(base, "result.json"))
    for path in candidates:
        if not os.path.isfile(path):
            continue
        try:
            with open(path, "r", encoding="utf-8") as f:
                payload = json.load(f)
        except Exception:
            continue
        verdict = str(payload.get("verdict", "")).upper()
        if verdict in _PIXEL_VERDICTS_CONFIRMED:
            return (True, path, verdict)
    # Report the last seen artifact (if any) even when not confirmed, for
    # diagnostic clarity in the summary.
    for path in reversed(candidates):
        if os.path.isfile(path):
            try:
                with open(path, "r", encoding="utf-8") as f:
                    payload = json.load(f)
                return (False, path, str(payload.get("verdict", "")).upper() or None)
            except Exception:
                continue
    return (False, None, None)

_SWAPCHAIN_CREATE_MARKER = "macrunner-hb-dxgi-swapchain: create method=CreateSwapChainForHwnd"

def time_to_swapchain_seconds(text):
    """Return seconds from lane runner start to the first real CreateSwapChainForHwnd.

    New laneA-run-hk.sh logs every mr-run line as:
      [laneA-ts epoch=<epoch> +<seconds>s] ...
    and also appends a post-run:
      [laneA] time_to_swapchain=<seconds>s ...
    Prefer the explicit post-run value, but tolerate direct timestamped marker
    parsing so older partially-written runs still classify.
    """
    explicit = re.search(r"\btime_to_swapchain=([0-9]+(?:\.[0-9]+)?)s\b", text)
    if explicit:
        return float(explicit.group(1))

    start_epoch = None
    start_match = re.search(r"\[laneA\] run_start_epoch=([0-9]+(?:\.[0-9]+)?)\b", text)
    if start_match:
        try:
            start_epoch = float(start_match.group(1))
        except ValueError:
            start_epoch = None

    for line in text.splitlines():
        if _SWAPCHAIN_CREATE_MARKER not in line:
            continue
        if not ("rc=0x0" in line or "rc=(nil)" in line or "rc=0x00000000" in line):
            continue
        rel = re.search(r"\+([0-9]+(?:\.[0-9]+)?)s\]", line)
        if rel:
            try:
                return float(rel.group(1))
            except ValueError:
                pass
        epoch = re.search(r"\[laneA-ts epoch=([0-9]+(?:\.[0-9]+)?)\b", line)
        if epoch and start_epoch is not None:
            try:
                return float(epoch.group(1)) - start_epoch
            except ValueError:
                pass
    return None

def _read_head_tail(path, head=1024 * 1024, tail=4 * 1024 * 1024):
    """First `head` + last `tail` bytes — late milestones (GfxDevice/D3D11)
    live at the END of 30MB run.logs; head-only reads miss them."""
    try:
        size = os.path.getsize(path)
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            if size <= head + tail:
                return f.read()
            data = f.read(head)
            f.seek(max(0, size - tail))
            return data + "\n" + f.read()
    except Exception:
        return ""

_REL_TS_RE = re.compile(r"\+([0-9]+(?:\.[0-9]+)?)s\]")

def _line_rel_seconds(line):
    m = _REL_TS_RE.search(line)
    if not m:
        return None
    try:
        return float(m.group(1))
    except ValueError:
        return None

def detect_post_swapchain_worker_thread_death(logs, combined_text, time_to_swapchain):
    """Streaming HK classifier: swapchain reached, GetBuffer absent, then a guest
    worker thread exits with c000007b/runtime after a MEMORY_FAULT and the run
    keeps spinning until the watchdog. This prevents the D3D gate from masking
    the real Lane-A thread-death blocker as PRESENT_MISSING."""
    paths = []
    for log_type in ("run", "stderr", "stdout"):
        path = logs.get(log_type)
        if path and os.path.exists(path) and path not in paths:
            paths.append(path)

    swap_t = time_to_swapchain
    getbuffer_seen = _swapchain_real_call_hit(
        combined_text, "macrunner-hb-dxgi-swapchain: method=GetBuffer slot=9")
    runtime_fault = None
    death = None
    late_heartbeat = None
    max_t = 0.0

    for path in paths:
        try:
            with open(path, "r", encoding="utf-8", errors="ignore") as f:
                for raw in f:
                    line = raw.rstrip("\n")
                    t = _line_rel_seconds(line)
                    if t is not None and t > max_t:
                        max_t = t
                    if swap_t is None and _SWAPCHAIN_CREATE_MARKER in line and (
                            "rc=0x0" in line or "rc=(nil)" in line or "rc=0x00000000" in line):
                        swap_t = t
                    if ("macrunner-hb-dxgi-swapchain: method=GetBuffer slot=9" in line and
                            "candidate" not in line and ("rc=0x0" in line or "rc=(nil)" in line or "rc=0x00000000" in line)):
                        getbuffer_seen = True
                    if ("macrunner-hb-runtime-fail: label=thread" in line and
                            "out=MEMORY_FAULT" in line):
                        runtime_fault = (t, line[:360])
                    if ("macrunner-hb-run-exit: label=thread" in line and
                            "status=c000007b" in line and "reason=runtime" in line):
                        delta = None if (t is None or swap_t is None) else t - swap_t
                        if swap_t is None or delta is None or (120.0 <= delta <= 400.0):
                            death = (t, delta, line[:360])
                    if death and t is not None and death[0] is not None and t > death[0] + 30.0:
                        if "macrunner-hb-heartbeat:" in line:
                            late_heartbeat = (t, line[:240])
        except Exception:
            continue

    watchdog_kill = "[mr-run] exit=143" in combined_text or " rc=143 " in combined_text
    if not death or getbuffer_seen:
        return None
    if swap_t is None:
        return None
    if not (runtime_fault or "MEMORY_FAULT" in death[2]):
        return None
    if not (watchdog_kill or late_heartbeat or (death[0] is not None and max_t > death[0] + 120.0)):
        return None

    evidence = [
        "[classify_run] swapchain reached at +%.1fs; GetBuffer not reached." % swap_t,
        "[classify_run] worker thread exited with c000007b/runtime%s: %s" %
        ("" if death[1] is None else " %.1fs after swapchain" % death[1], death[2]),
    ]
    if runtime_fault:
        evidence.append("[classify_run] matching thread MEMORY_FAULT: %s" % runtime_fault[1])
    if late_heartbeat:
        evidence.append("[classify_run] run continued after worker death; late heartbeat at +%.1fs: %s" %
                        (late_heartbeat[0], late_heartbeat[1]))
    if watchdog_kill:
        evidence.append("[classify_run] watchdog exit=143 after the worker death, consistent with main-loop wait.")

    return {
        "verdict": "BLOCKED",
        "owner": "Lane A",
        "class": "POST_SWAPCHAIN_WORKER_THREAD_DEATH",
        "confidence": 0.92,
        "evidence": evidence,
        "next_action": (
            "Deliver guest SEH AV for HyperBridge MEMORY_FAULT instead of returning "
            "STATUS_INVALID_IMAGE_FORMAT from macrunner_hb_run_x64; if delivery fails, "
            "store-watch the corrupted Unity descriptor slot."
        ),
        "missing_markers": {"critical": ["GetBuffer"], "secondary": []},
        "next_run_env": {"MACRUNNER_HB_GUEST_AV_DELIVERY": "1"}
    }

FAULT_PATTERNS = ["MEMORY_FAULT", "err:seh", "c0000005", "Unhandled exception",
                  "runtime-fail", "callback-route-reject", "SIGBUS", "SIGSEGV"]
LANE_A_OWNER_HINTS = ["arm64ec", "macrunner-hb-", "xtajit64", "ldr-init",
                      "callback-route", "dispatcher", "signal_arm64",
                      "init-frame", "entry thunk", "__os_arm64x"]

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 classify_run.py <run-directory>")
        sys.exit(1)

    run_dir = sys.argv[1]
    if not os.path.exists(run_dir):
        print(f"Error: path '{run_dir}' does not exist.")
        sys.exit(1)

    run_dir_abs = os.path.abspath(run_dir)
    logs = find_logs(run_dir_abs)

    # Report flight.jsonl availability for diagnostic clarity
    if logs.get("flight"):
        print(f"[classify_run] flight.jsonl found: {logs['flight']}")
    else:
        print(
            "[classify_run] WARNING: no flight.jsonl in run directory — analyzers fall back to "
            "run.log line parsing; confidence may be lower. "
            "To capture flight data, set MACRUNNER_FLIGHT_RECORDER=1 when running mr-run.sh "
            "and ensure the flight path is inside the run directory "
            "(see reports/research/TRIAGE-NEEDS.md for the harness fix)."
        )

    # Contextual check for WOW64 and D3D activity markers
    wow64_markers_exist = False
    d3d_markers_exist = False
    combined_log_text = ""
    for log_type in ["run", "stderr", "stdout"]:
        fpath = logs.get(log_type)
        if fpath and os.path.exists(fpath):
            combined_log_text += _read_head_tail(fpath) + "\n"
    prefix_path = os.path.join(run_dir_abs, "prefix_check.txt")
    if os.path.exists(prefix_path):
        try:
            with open(prefix_path, "r", encoding="utf-8", errors="ignore") as f:
                combined_log_text += f.read(1024 * 1024)
        except: pass
    log_lower = combined_log_text.lower()
    if any(m in log_lower for m in ["wow64", "wow64cpu", "btcpu", "pe32", "wow64_init"]):
        wow64_markers_exist = True
    if any(m in log_lower for m in ["d3d11", "dxgi", "wined3d", "dxmt", "gfxdevice"]):
        d3d_markers_exist = True

    if logs.get("flight"):
        try:
            for ev in stream_events(logs):
                ev_str = str(ev).lower()
                if any(m in ev_str for m in ["wow64", "wow64cpu", "btcpu", "pe32", "wow64_init"]):
                    wow64_markers_exist = True
                if any(m in ev_str for m in ["d3d11", "dxgi", "wined3d", "dxmt", "gfxdevice"]):
                    d3d_markers_exist = True
        except: pass

    script_dir = os.path.dirname(os.path.abspath(__file__))
    results = {}
    any_blocked = False
    any_unknown = False
    all_pass = True

    print(f"Running analyzers against directory: {run_dir_abs}\n")

    for analyzer in ANALYZERS:
        analyzer_path = os.path.join(script_dir, analyzer)
        if not os.path.exists(analyzer_path):
            print(f"Warning: Analyzer {analyzer} not found at {analyzer_path}")
            continue

        cmd = [sys.executable, analyzer_path, run_dir_abs]
        try:
            res = subprocess.run(cmd, capture_output=True, text=True, check=True)
            parsed = parse_analyzer_output(res.stdout)
            results[analyzer] = parsed

            # Check verdict flags
            v = parsed["verdict"].upper()
            if v == "BLOCKED":
                any_blocked = True
                all_pass = False
            elif v == "UNKNOWN":
                any_unknown = True
                all_pass = False
            elif v != "PASS":
                all_pass = False

            print(f"[{analyzer}] VERDICT={parsed['verdict']} CLASS={parsed['class']} CONFIDENCE={parsed['confidence']:.2f}")
        except subprocess.CalledProcessError as e:
            results[analyzer] = {
                "verdict": "UNKNOWN",
                "owner": "Unknown",
                "class": "ANALYZER_CRASHED",
                "confidence": 0.0,
                "evidence": [f"Subprocess failed with code {e.returncode}"],
                "next_action": f"Fix analyzer crash: {analyzer}",
                "missing_markers": {"critical": [], "secondary": []},
                "next_run_env": {}
            }
            any_unknown = True
            all_pass = False

    # Apply cross-analyzer contextual demotion rules
    for name, r in list(results.items()):
        klass = r["class"]
        verdict = r["verdict"].upper()
        
        # Demote PE32 WOW64 handoff if no WOW64 markers exist
        if klass.startswith("PE32_") and verdict == "BLOCKED" and not wow64_markers_exist:
            r["verdict"] = "UNKNOWN"
            r["class"] = "PE32_HANDOFF_TRACE_INSUFFICIENT"
            r["confidence"] = 0.30
            r["next_action"] = "Capture a flight recorder log with WOW64 and BTCPU handoff tracing enabled."
            r["evidence"].append("[classify_run] Demoted from BLOCKED because no PE32/WOW64 activity was detected in the logs.")
            
        # Demote D3D blockers if no D3D markers exist
        d3d_blocker_classes = [
            "D3D11_DLL_NOT_LOADED", "DXGI_DLL_NOT_LOADED", "CREATE_DXGI_FACTORY_MISSING",
            "D3D11_CREATE_DEVICE_MISSING", "PRESENT_MISSING", "WINED3D_FALLBACK"
        ]
        if (klass in d3d_blocker_classes or klass.startswith("D3D11_")) and verdict == "BLOCKED" and not d3d_markers_exist:
            r["verdict"] = "UNKNOWN"
            r["class"] = "D3D_TRACE_INSUFFICIENT"
            r["confidence"] = 0.30
            r["next_action"] = "Capture a flight recorder log with D3D and graphics gate logging enabled."
            r["evidence"].append("[classify_run] Demoted from BLOCKED because no D3D/graphics activity was detected in the logs.")

    # --- DLL-load / system-DLL-init supersedes downstream graphics+window FP ---
    # When the boot dies at DLL load or system-DLL init (BEFORE the graphics pipeline
    # can start), the d3d gate mis-reads a stray wined3d.dll import-prefix line as a
    # WineD3D fallback and the window gate mis-reads a load_dll c0000135 line as a
    # window-server error. Both are false positives: the run never reached that stage.
    # Demote them so the coordinator sees the real earlier-stage blocker instead.
    _EARLY_BLOCKER_CLASSES = ("DLL_LOAD_FAILURE", "SYSTEM_DLL_INIT_CASCADE")
    _D3D_FP_CLASSES = [
        "D3D11_DLL_NOT_LOADED", "DXGI_DLL_NOT_LOADED", "CREATE_DXGI_FACTORY_MISSING",
        "D3D11_CREATE_DEVICE_MISSING", "PRESENT_MISSING", "WINED3D_FALLBACK",
    ]
    _LOAD_INIT_SIGS = (
        "macrunner-ldr-load-dll-fail", "macrunner-ldr-ldrloaddll-fail",
        "map_fixed_area", "loader_init", "c0000135", "c0000005",
        "dispatch_exception", "macrunner-hb-first-chance",
    )
    early_blocker_present = any(
        rr["verdict"].upper() == "BLOCKED" and rr["class"] in _EARLY_BLOCKER_CLASSES
        for rr in results.values()
    )
    if early_blocker_present:
        for rname, rr in list(results.items()):
            klass2 = rr["class"]
            verdict2 = rr["verdict"].upper()
            if verdict2 != "BLOCKED" or klass2 in _EARLY_BLOCKER_CLASSES:
                continue
            suppress = False
            reason = ""
            if klass2 in _D3D_FP_CLASSES or klass2.startswith("D3D11_"):
                # Graphics never ran if the boot died at DLL load/init — the d3d
                # gate's "no real graphics markers" verdict is itself the proof.
                suppress = True
                reason = ("graphics pipeline never started: boot died at DLL load / "
                          "system-DLL init, so the absence of real D3D markers is the "
                          "expected consequence, not a backend-selection failure")
            elif klass2 in ("WINDOW_SERVER_ERROR", "GENERIC_ACCESS_VIOLATION",
                            "GENERIC_WOW64_FAULT", "WINDOW_NCCREATE_ABORTED"):
                # Only suppress when this class's evidence is itself load/init noise
                # (e.g. the window gate latched onto a load_dll c0000135 line).
                ev_lines = " ".join(rr.get("evidence", []))
                if ev_lines and any(sig in ev_lines.lower() for sig in _LOAD_INIT_SIGS):
                    suppress = True
                    reason = ("evidence is load/init noise (c0000135/c0000005 from "
                              "LdrLoadDll/loader_init), not a genuine %s" % klass2)
            if suppress:
                rr["verdict"] = "UNKNOWN"
                rr["class"] = klass2 + "_SUPERSEDED_BY_DLL_LOAD"
                rr["confidence"] = 0.20
                rr["evidence"].append(
                    "[classify_run] Superseded by an earlier-stage DLL_LOAD_FAILURE / "
                    "SYSTEM_DLL_INIT_CASCADE blocker: " + reason)

    # --- Boot-ladder: furthest rung this run + regression vs persisted best ---
    rung_idx, rung_name = ladder_rung(combined_log_text)
    state_path = os.path.join(os.path.dirname(run_dir_abs), ".triage-ladder-best.json")
    ladder_best = {}
    try:
        if os.path.exists(state_path):
            with open(state_path, "r", encoding="utf-8") as f:
                ladder_best = json.load(f)
    except Exception:
        ladder_best = {}
    best_idx = int(ladder_best.get("rung_idx", -1))
    best_name = ladder_best.get("rung_name")
    best_run = ladder_best.get("run", "?")
    ladder_regression = (rung_idx >= 0 and best_idx >= 0 and rung_idx < best_idx)

    # Pixel-truth-gate artifact confirmation (orthogonal to the call-level
    # ladder). PIXEL_MOMENT_REACHED requires BOTH rung 14 (real-present call
    # marker on a known swapchain, rc=0) AND a pixel-truth-gate result.json
    # artifact with verdict NONBLACK/COLORFUL inside the run dir. A real
    # Present call with no pixel artifact = call reached, frame NOT confirmed.
    px_confirmed, px_artifact, px_verdict = pixel_truth_confirmed(run_dir_abs)
    pixel_moment_reached = (rung_idx == 14 and px_confirmed)
    time_to_swapchain = time_to_swapchain_seconds(combined_log_text)
    worker_thread_death = detect_post_swapchain_worker_thread_death(
        logs, combined_log_text, time_to_swapchain)
    if rung_idx > best_idx:
        try:
            with open(state_path, "w", encoding="utf-8") as f:
                json.dump({"rung_idx": rung_idx, "rung_name": rung_name,
                           "run": os.path.basename(run_dir_abs),
                           "ladder": [n for n, _ in LADDER_RUNGS]}, f, indent=2)
        except Exception:
            pass
        best_idx, best_name, best_run = rung_idx, rung_name, os.path.basename(run_dir_abs)
        print(f"** NEW LADDER MILESTONE: '{rung_name}' (rung {rung_idx}). ARCHIVE the working "
              f"deployed binaries NOW — git holds source, not the deployed combination (the 06-07 "
              f"GfxDevice dist was lost to an overnight redeploy): "
              f"scripts/milestone-dist-snapshot.sh {rung_name}")

    def get_priority_score(r):
        verdict = r["verdict"].upper()
        klass = r["class"]
        
        if verdict == "BLOCKED":
            if klass in ("WINDOW_THREAD_DESKTOP_ZERO", "WINDOW_NO_DESKTOP"):
                return 100
            elif klass == "WINDOW_NCCREATE_ABORTED":
                return 95
            elif klass == "WINDOW_SEQUENCE_VIOLATION":
                return 92
            elif klass == "WINDOW_SERVER_ERROR":
                return 90
            elif klass == "GFXDEVICE_SEH_HOST_BOUNDARY_BEFORE_D3D11":
                return 89
            elif klass in ("SMC_STALE_TB_EXECUTION", "FLUSH_ICACHE_NO_INVALIDATE", "VIRTUALPROTECT_EXEC_NO_INVALIDATE"):
                return 88
            elif klass in ("CLASS_INSTANCE_MISMATCH", "CLASS_ATOM_MISMATCH"):
                return 85
            elif klass in ("CREATESTRUCT_BAD_POINTER", "CREATESTRUCT_FIELD_CORRUPTION"):
                return 82
            elif klass == "WNDPROC_RETURNED_FALSE":
                return 80
            elif klass.startswith("PE32_"):
                return 75
            elif klass in ("DLL_LOAD_FAILURE",):
                return 96
            elif klass == "SYSTEM_DLL_INIT_CASCADE":
                return 94
            elif klass == "POST_SWAPCHAIN_WORKER_THREAD_DEATH":
                return 91
            elif klass in ("D3D11_CREATE_DEVICE_MISSING", "WINED3D_FALLBACK", "D3D11_DLL_NOT_LOADED", "DXGI_DLL_NOT_LOADED", "CREATE_DXGI_FACTORY_MISSING", "PRESENT_MISSING") or klass.startswith("D3D11_"):
                return 70
            elif klass.startswith("ARM64EC_"):
                return 65
            elif klass == "SILENT_SPIN_NO_MARKERS":
                return 62
            elif klass.startswith("WAIT_"):
                return 60
            elif klass in ("WINDOW_HANDLE_CREATE_FAILED", "WINDOW_MESSAGES_MISSING"):
                return 55
            elif klass.startswith("GENERIC_"):
                return 30
            else:
                return 50
        elif verdict == "UNKNOWN":
            if klass.endswith("_TRACE_INSUFFICIENT"):
                return 20
            elif klass.startswith("GENERIC_"):
                return 15
            else:
                return 10
        elif verdict == "PASS":
            return 5
        else:
            return 0

    # Pick the selected verdict based on priority score, then confidence
    non_pass_results = []
    for name, r in results.items():
        if r["verdict"].upper() != "PASS":
            non_pass_results.append((name, r))

    selected_result = None
    why_primary_won = ""
    secondary_classes = []

    if non_pass_results:
        non_pass_sorted = sorted(non_pass_results, key=lambda x: (get_priority_score(x[1]), x[1]["confidence"]), reverse=True)
        _, selected_result = non_pass_sorted[0]

        # A confident PASS (definite forward progress, e.g. PE32_I386_EXECUTING) should headline over a
        # mere UNKNOWN / *_TRACE_INSUFFICIENT ("found nothing specific") — but NEVER over a real BLOCKED.
        if selected_result["verdict"].upper() == "UNKNOWN":
            confident_pass = [r for _, r in results.items()
                              if r["verdict"].upper() == "PASS" and float(r.get("confidence", 0) or 0) >= 0.85]
            if confident_pass:
                confident_pass.sort(key=lambda r: float(r.get("confidence", 0) or 0), reverse=True)
                selected_result = confident_pass[0]

        # Calculate primary won details
        if len(non_pass_sorted) > 1:
            other_classes = [r["class"] for _, r in non_pass_sorted[1:]]
            secondary_classes = other_classes
            why_primary_won = f"Primary class '{selected_result['class']}' (priority {get_priority_score(selected_result)}) outranked secondary classes: {', '.join(other_classes)}."
        else:
            why_primary_won = f"Only class '{selected_result['class']}' was identified as non-PASS."
    else:
        # All are PASS (or empty)
        pass_results = [(name, r) for name, r in results.items() if r["verdict"].upper() == "PASS"]
        if pass_results:
            pass_sorted = sorted(pass_results, key=lambda x: x[1]["confidence"], reverse=True)
            _, selected_result = pass_sorted[0]
            why_primary_won = "All analyzers reported PASS."
        else:
            selected_result = {
                "verdict": "UNKNOWN",
                "owner": "Unknown",
                "class": "NO_ANALYZER_RESULTS",
                "confidence": 0.0,
                "evidence": [],
                "next_action": "Ensure analyzers are present and run them on a valid log directory.",
                "missing_markers": {"critical": [], "secondary": []},
                "next_run_env": {}
            }
            why_primary_won = "No analyzer returned results."

    if worker_thread_death and (
            selected_result["class"] in ("PRESENT_MISSING", "D3D11_CREATE_DEVICE_MISSING",
                                         "WINED3D_FALLBACK", "D3D11_DLL_NOT_LOADED",
                                         "DXGI_DLL_NOT_LOADED", "CREATE_DXGI_FACTORY_MISSING") or
            selected_result["class"].startswith("D3D11_") or
            float(selected_result.get("confidence", 0) or 0) < worker_thread_death["confidence"]):
        previous = selected_result
        selected_result = worker_thread_death
        secondary_classes = [previous["class"]] + secondary_classes
        why_primary_won = (
            "Overridden by classify_run: post-swapchain x64 worker thread death "
            "is upstream of the downstream graphics/PRESENT_MISSING symptom.")

    # --- SILENT_SPIN_NO_MARKERS: watchdog kill + ZERO faults + no forward markers ---
    watchdog_kill = "[mr-run] exit=143" in combined_log_text
    any_fault = any(p in combined_log_text for p in FAULT_PATTERNS)
    if (watchdog_kill and not any_fault
            and float(selected_result.get("confidence", 0) or 0) <= 0.55
            and (selected_result["class"].startswith("GENERIC_")
                 or selected_result["class"].endswith("_TRACE_INSUFFICIENT")
                 or selected_result["verdict"].upper() == "UNKNOWN")):
        selected_result = dict(selected_result)
        selected_result["verdict"] = "BLOCKED"
        selected_result["class"] = "SILENT_SPIN_NO_MARKERS"
        selected_result["confidence"] = 0.70
        selected_result["evidence"] = (selected_result.get("evidence") or [])[:5] + [
            "[classify_run] watchdog kill (exit=143) with ZERO fault markers and no boot-ladder "
            "progress past rung '%s' — silent host-side spin/self-loop." % (rung_name,)]
        selected_result["next_action"] = (
            "Silent spin: no fault, no forward markers, watchdog kill. Rerun and SAMPLE the wine "
            "process mid-run (~50% of timeout): `ps ax -o pid,command | grep -i 'Hollow Knight.exe'` "
            "then `sample <pid> 5 -file $RUNDIR/sample.txt`; the pc-histogram top self-loop rva names "
            "the spin (e.g. 2026-06-10: __wine_unix_call_arm64ec dispatcher-slot self-loop rva "
            "0xe72e0-e72e8, root = unix loader.c GET_FUNC gated on is_arm64ec()).")
        why_primary_won = "Overridden by classify_run: watchdog-kill with zero faults/zero markers = silent spin."

    # --- Owner correction: GENERIC_*/silent-spin with dispatch/EC-boundary markers = Lane A ---
    if (selected_result["class"].startswith("GENERIC_")
            or selected_result["class"] == "SILENT_SPIN_NO_MARKERS"):
        low_all = (combined_log_text + " " + " ".join(selected_result.get("evidence") or [])).lower()
        if any(h in low_all for h in LANE_A_OWNER_HINTS) and selected_result.get("owner") != "Lane A":
            selected_result = dict(selected_result)
            selected_result["owner"] = "Lane A"
            selected_result["evidence"] = list(selected_result.get("evidence") or []) + [
                "[classify_run] Owner corrected to Lane A: ARM64EC/dispatch/ldr-init boundary markers present."]

    # Print summary report
    print("\n" + "="*40)
    print("FINAL TRIAGE SUMMARY")
    print("="*40)
    print(f"VERDICT: {selected_result['verdict']}")
    print(f"OWNER:   {selected_result['owner']}")
    print(f"CLASS:   {selected_result['class']}")
    print(f"CONFIDENCE: {selected_result['confidence']:.2f}")
    if rung_idx >= 0:
        print(f"LADDER_RUNG: {rung_idx} ({rung_name})  BEST: {best_idx} ({best_name}) run={best_run}")
        if ladder_regression:
            print(f"!! LADDER_REGRESSION: this run stopped at '{rung_name}' but best-ever reached "
                  f"'{best_name}' ({best_run}). Suspect the change under test regressed an earlier "
                  f"stage (or tracing was reduced) — restore/verify the last verified-forward "
                  f"baseline BEFORE iterating further.")
    print(f"time_to_swapchain={int(round(time_to_swapchain))}s" if time_to_swapchain is not None
          else "time_to_swapchain=UNKNOWN")
    # Pixel-truth reporting. PIXEL_MOMENT_REACHED is the only flag that credits
    # an actual drawn frame on rungs 12-14; the call-level ladder alone never
    # does (operator rule: frame gated ONLY via pixel-truth-gate.sh).
    px_status = "CONFIRMED" if px_confirmed else ("NO_ARTIFACT" if not px_artifact else "NOT_CONFIRMED")
    print(f"PIXEL_TRUTH: {px_status} verdict={px_verdict} artifact={px_artifact}")
    print(f"PIXEL_MOMENT_REACHED: {'yes' if pixel_moment_reached else 'no'}")
    if rung_idx == 14 and not px_confirmed:
        print(f"!! PIXEL_PENDING: real-Present call marker reached (rung 14) but no "
              f"pixel-truth-gate artifact confirms a drawn frame. Run: "
              f"scripts/pixel-truth-gate.sh --pid <PID> --rundir <run-dir>/pixel-truth-gate "
              f"(or --watch) BEFORE crediting a pixel.")
    print(f"PRIMARY_CLASS={selected_result['class']}")
    print(f"SECONDARY_CLASSES={', '.join(secondary_classes)}")
    print(f"WHY_PRIMARY_WON={why_primary_won}")
    print("EVIDENCE:")
    for line in selected_result["evidence"][:10]:
        print(f"  {line}")
    print("NEXT_ACTION:")
    print(f"  {selected_result['next_action']}")
    print("="*40)

    # Save summary files inside run-dir if it's a directory
    if os.path.isdir(run_dir_abs):
        summary_txt_path = os.path.join(run_dir_abs, "triage-summary.txt")
        summary_json_path = os.path.join(run_dir_abs, "triage-summary.json")

        # Save txt summary exactly as Task 11 specifies
        try:
            with open(summary_txt_path, "w", encoding="utf-8") as f:
                f.write(f"OWNER: {selected_result['owner']}\n")
                f.write(f"CLASS: {selected_result['class']}\n")
                f.write(f"CONFIDENCE: {selected_result['confidence']:.2f}\n")
                if rung_idx >= 0:
                    f.write(f"LADDER_RUNG: {rung_idx} ({rung_name})\n")
                    f.write(f"LADDER_BEST: {best_idx} ({best_name}) run={best_run}\n")
                    f.write(f"LADDER_REGRESSION: {'YES — restore last verified-forward baseline' if ladder_regression else 'no'}\n")
                f.write(f"time_to_swapchain={int(round(time_to_swapchain))}s\n" if time_to_swapchain is not None
                        else "time_to_swapchain=UNKNOWN\n")
                f.write(f"PIXEL_TRUTH: {'CONFIRMED' if px_confirmed else ('NO_ARTIFACT' if not px_artifact else 'NOT_CONFIRMED')} verdict={px_verdict} artifact={px_artifact}\n")
                f.write(f"PIXEL_MOMENT_REACHED: {'yes' if pixel_moment_reached else 'no'}\n")
                f.write(f"WHY_PRIMARY_WON: {why_primary_won}\n")
                f.write("EVIDENCE:\n")
                for line in selected_result["evidence"][:10]:
                    f.write(f"  {line}\n")
                
                missing = selected_result.get("missing_markers", {})
                if missing:
                    crit = missing.get("critical", [])[:2]
                    sec = missing.get("secondary", [])[:3]
                    if crit or sec:
                        f.write("MISSING_MARKERS:\n")
                        if crit:
                            f.write("  CRITICAL:\n")
                            for m in crit:
                                f.write(f"    - {m}\n")
                        if sec:
                            f.write("  SECONDARY:\n")
                            for m in sec:
                                f.write(f"    - {m}\n")
                
                f.write(f"NEXT_ACTION: {selected_result['next_action']}\n")
                
                env = selected_result.get("next_run_env", {})
                if env:
                    f.write("NEXT_RUN_ENV:\n")
                    for k, v in env.items():
                        f.write(f"  {k}={v}\n")
                
                f.write(f"SECONDARY_CLASSES: {', '.join(secondary_classes)}\n")
            print(f"Saved text summary to: {summary_txt_path}")
        except Exception as e:
            print(f"Warning: Failed to save triage-summary.txt: {e}", file=sys.stderr)

        # Save json summary
        try:
            summary_data = {
                "final_verdict": selected_result["verdict"],
                "final_owner": selected_result["owner"],
                "final_class": selected_result["class"],
                "final_confidence": selected_result["confidence"],
                "final_next_action": selected_result["next_action"],
                "why_primary_won": why_primary_won,
                "ladder_rung": {"idx": rung_idx, "name": rung_name},
                "ladder_best": {"idx": best_idx, "name": best_name, "run": best_run},
                "ladder_regression": ladder_regression,
                "time_to_swapchain_sec": time_to_swapchain,
                "pixel_truth": {"confirmed": px_confirmed, "artifact": px_artifact, "verdict": px_verdict},
                "pixel_moment_reached": pixel_moment_reached,
                "secondary_classes": secondary_classes,
                "missing_markers": selected_result.get("missing_markers", {}),
                "next_run_env": selected_result.get("next_run_env", {}),
                "analyzers": results
            }
            with open(summary_json_path, "w", encoding="utf-8") as f:
                json.dump(summary_data, f, indent=2)
            print(f"Saved JSON summary to: {summary_json_path}")
        except Exception as e:
            print(f"Warning: Failed to save triage-summary.json: {e}", file=sys.stderr)

    # SELF_CHECK validation
    self_check = "PASS"
    self_check_reason = ""
    
    klass_check = selected_result["class"]
    evidence_check = selected_result["evidence"]
    missing_check = selected_result.get("missing_markers", {})
    env_check = selected_result.get("next_run_env", {})
    
    if klass_check == "WINDOW_NCCREATE_ABORTED_TRACE_INSUFFICIENT":
        crit = missing_check.get("critical", [])
        sec = missing_check.get("secondary", [])
        if not crit and not sec:
            self_check = "FAIL"
            self_check_reason = "missing_markers is empty for trace insufficient class"
        elif not env_check:
            self_check = "FAIL"
            self_check_reason = "next_run_env is empty for trace insufficient class"
        else:
            self_check_reason = "trace insufficient markers and environment are correctly prescribed"
    else:
        # More specific class
        if not evidence_check:
            self_check = "FAIL"
            self_check_reason = "evidence is empty for specific class"
        else:
            # Check if evidence has real log lines
            has_real_log = False
            for ev_item in evidence_check:
                if "macrunner-" in ev_item or "wndproc_" in ev_item or "register_class" in ev_item or "create_window" in ev_item or "create-window-handle" in ev_item or "[classify_run]" in ev_item:
                    has_real_log = True
                    break
            if not has_real_log:
                self_check = "FAIL"
                self_check_reason = "evidence does not contain any real log lines"
            else:
                self_check_reason = f"specific class '{klass_check}' validated with real log evidence"
                
    print(f"SELF_CHECK={self_check}")
    print(f"SELF_CHECK_REASON={self_check_reason}")

    # Exit codes: 0=all PASS · 1=any BLOCKED · 2=only UNKNOWN
    if any_blocked:
        sys.exit(1)
    elif any_unknown:
        sys.exit(2)
    elif all_pass:
        sys.exit(0)
    else:
        sys.exit(0)

if __name__ == "__main__":
    main()
