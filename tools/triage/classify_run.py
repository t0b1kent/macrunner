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
            try:
                with open(fpath, "r", encoding="utf-8", errors="ignore") as f:
                    combined_log_text += f.read(1024 * 1024)
            except: pass
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

    # Print summary report
    print("\n" + "="*40)
    print("FINAL TRIAGE SUMMARY")
    print("="*40)
    print(f"VERDICT: {selected_result['verdict']}")
    print(f"OWNER:   {selected_result['owner']}")
    print(f"CLASS:   {selected_result['class']}")
    print(f"CONFIDENCE: {selected_result['confidence']:.2f}")
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
                if "macrunner-" in ev_item or "wndproc_" in ev_item or "register_class" in ev_item or "create_window" in ev_item or "create-window-handle" in ev_item:
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
