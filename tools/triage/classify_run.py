import sys
import os
import subprocess
import json

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
    ("present",        ["present_reached=YES", "Present hr=0", "present_count="]),
    ("window-visible", ["window-visible", "CG-capture", "non_background_pixels"]),
]

def _marker_hit(text, marker):
    """Substring hit, but reject counter lines like 'D3D11CreateDevice=0' /
    'GfxDevice_count=...' (char right after the marker must not be = or _)."""
    start = 0
    while True:
        i = text.find(marker, start)
        if i < 0:
            return False
        j = i + len(marker)
        if text[j:j + 1] not in ("=", "_"):
            return True
        start = j

def ladder_rung(text):
    """(idx, name) of the FURTHEST rung whose marker appears; (-1, None) if none."""
    best = (-1, None)
    for idx, (name, markers) in enumerate(LADDER_RUNGS):
        if any(_marker_hit(text, m) for m in markers):
            best = (idx, name)
    return best

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
