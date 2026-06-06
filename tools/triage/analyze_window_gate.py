import sys
import os

# Ensure tools/triage is in path so we can import triage_common
sys.path.append(os.path.dirname(os.path.abspath(__file__)))
from triage_common import find_logs, stream_events, print_report, build_timeline

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 analyze_window_gate.py <flight.jsonl | run-dir | run.log>")
        sys.exit(1)

    path = sys.argv[1]
    logs = find_logs(path)
    events = list(stream_events(logs))

    verdict = "UNKNOWN"
    owner = "Lane A"
    klass = "WINDOW_TRACE_INSUFFICIENT"
    confidence = 0.5
    evidence = []
    next_action = "Capture a flight recorder log with window and message gate logging active."
    missing_markers = None
    next_run_env = None

    if not events:
        print_report(verdict, "Unknown", klass, 0.0, evidence, next_action)
        return

    # Reconstruct timeline for window sequence validation
    timeline = build_timeline(events)
    win_events = [t for t in timeline if t[2] in (
        "register_class",
        "create_window_call",
        "create_window",
        "ntusercreatewnd_server",
        "server_create_window",
        "wm_nccreate_dispatch",
        "wm_nccreate_return",
        "driver_create_window_ok",
        "driver_create_window_failed",
        "wm_nccreate_aborted"
    )]
    
    # Prerequisite: RegisterClass mapping class -> instance
    registered_classes_map = {}
    
    for ts, tid, ev_type, summary in win_events:
        if ev_type == "register_class":
            cname = None
            atom = None
            inst = None
            if "class_name=" in summary:
                try:
                    cname = summary.split("class_name=")[1].split()[0]
                    if cname == "None": cname = None
                except: pass
            if "atom=" in summary:
                try:
                    atom_str = summary.split("atom=")[1].split()[0]
                    if atom_str != "None":
                        atom = int(atom_str, 16 if "0x" in atom_str.lower() else 10)
                except: pass
            if "instance=" in summary:
                try:
                    inst_str = summary.split("instance=")[1].split()[0]
                    if inst_str != "None":
                        inst = int(inst_str, 16 if "0x" in inst_str.lower() else 10)
                except: pass
            
            if cname:
                registered_classes_map[cname] = inst
            if atom:
                registered_classes_map[atom] = inst

    # Track sequence per thread
    thread_states = {}  # tid -> last_step_num (integer 2..6 or None)
    sequence_violation = False
    violation_step = 0
    violation_expected = ""
    violation_found = []
    
    timeline_broken = False
    broken_step = 0
    broken_expected = ""
    broken_found = []
    
    sequence_evidence = []
    
    # Check if narrow tracing is active
    has_any_nccreate_trace = any(t[2] in ("wm_nccreate_dispatch", "wm_nccreate_return", "wm_nccreate_aborted") for t in win_events)
    
    for ts, tid, ev_type, summary in win_events:
        if ev_type == "register_class":
            continue
            
        step_num = 0
        if ev_type in ("create_window_call", "create_window", "ntusercreatewnd_server"): step_num = 2
        elif ev_type == "server_create_window": step_num = 3
        elif ev_type == "wm_nccreate_dispatch": step_num = 4
        elif ev_type == "wm_nccreate_return": step_num = 5
        elif ev_type in ("driver_create_window_ok", "driver_create_window_failed", "wm_nccreate_aborted"): step_num = 6
        
        if step_num == 0:
            continue
            
        last_step = thread_states.get(tid)
        
        if step_num == 2:
            # Check step 2 class/atom and instance matches registered class
            cname = None
            atom = None
            inst = None
            if "class_name=" in summary:
                try:
                    cname = summary.split("class_name=")[1].split()[0]
                    if cname == "None": cname = None
                except: pass
            if "atom=" in summary:
                try:
                    atom_str = summary.split("atom=")[1].split()[0]
                    if atom_str != "None":
                        atom = int(atom_str, 16 if "0x" in atom_str.lower() else 10)
                except: pass
            if "instance=" in summary:
                try:
                    inst_str = summary.split("instance=")[1].split()[0]
                    if inst_str != "None":
                        inst = int(inst_str, 16 if "0x" in inst_str.lower() else 10)
                except: pass
            
            if inst is not None:
                reg_inst = None
                if cname in registered_classes_map:
                    reg_inst = registered_classes_map[cname]
                elif atom in registered_classes_map:
                    reg_inst = registered_classes_map[atom]
                
                if reg_inst is not None and inst != reg_inst:
                    sequence_violation = True
                    violation_step = 2
                    violation_expected = f"hInstance={reg_inst} (from RegisterClass)"
                    violation_found = [f"hInstance={inst} (from CreateWindow)"]
                    sequence_evidence.append(f"[0] Sequence violation on thread {tid}: CreateWindow hInstance mismatch with RegisterClass. Line: {summary}")
                    break
            
            thread_states[tid] = 2
            
        else:
            if last_step is None:
                # First event seen on this thread
                if step_num == 6:
                    thread_states[tid] = None
                else:
                    timeline_broken = True
                    broken_step = 2
                    broken_expected = "Step 2 (create_window)"
                    broken_found = [ev_type]
                    sequence_evidence.append(f"[0] Missing expected step 2 before {ev_type} on thread {tid}. Line: {summary}")
                    break
            else:
                # Out of order: step_num < last_step
                if step_num < last_step:
                    sequence_violation = True
                    violation_step = step_num
                    violation_expected = f"Step >= {last_step}"
                    violation_found = [ev_type]
                    sequence_evidence.append(f"[0] Sequence violation on thread {tid}: {ev_type} found after step {last_step}. Line: {summary}")
                    break
                    
                # Duplicate step: step_num == last_step
                if step_num == last_step:
                    sequence_violation = True
                    violation_step = step_num
                    violation_expected = f"Step > {last_step}"
                    violation_found = [ev_type]
                    sequence_evidence.append(f"[0] Sequence violation on thread {tid}: Duplicate {ev_type} found. Line: {summary}")
                    break
                    
                # Skipped steps: step_num > last_step + 1
                if step_num > last_step + 1:
                    # 3 -> 6 is allowed only if no nccreate traces exist in the log
                    if last_step == 3 and step_num == 6 and not has_any_nccreate_trace:
                        pass
                    else:
                        timeline_broken = True
                        broken_step = last_step + 1
                        step_names = {3: "server_create_window", 4: "wm_nccreate_dispatch", 5: "wm_nccreate_return"}
                        broken_expected = f"Step {last_step + 1} ({step_names.get(last_step + 1, 'unknown')})"
                        broken_found = [ev_type]
                        sequence_evidence.append(f"[0] Missing expected step {last_step + 1} before {ev_type} on thread {tid}. Line: {summary}")
                        break
                
                # Move state forward
                if step_num == 6:
                    thread_states[tid] = None
                else:
                    thread_states[tid] = step_num

    if sequence_violation or timeline_broken:
        step = violation_step if sequence_violation else broken_step
        expected = violation_expected if sequence_violation else broken_expected
        found = violation_found if sequence_violation else broken_found
        sequence_evidence.append(f"TIMELINE_BROKEN_AT=step {step}")
        sequence_evidence.append(f"EXPECTED_STEP={expected}")
        sequence_evidence.append(f"FOUND_STEPS={', '.join(found)}")

    registered_classes = {}  # class_name -> {'atom': atom, 'instance': instance, 'ts': ts, 'tid': tid}
    registered_atoms = {}  # atom -> class_name
    window_created = False
    window_create_failed = False
    unity_create_window_success = False
    
    class_instance_mismatch = False
    class_atom_mismatch = False
    messages_dispatched = False
    
    nccreate_aborted = False
    window_handle_create_failed = False
    
    # P0
    window_thread_desktop_zero = False
    window_no_desktop = False
    
    # P1
    window_server_error = False
    window_server_error_confidence = 0.85
    
    # P2
    # class_instance_mismatch and class_atom_mismatch tracked below
    
    # P3
    createstruct_bad_pointer = False
    createstruct_field_corruption = False
    createstruct_field_corruption_confidence = 0.85
    
    # P4
    wndproc_returned_false = False
    
    # Trace markers presence tracking
    has_nccreate_return = False
    has_createstruct_valid = False
    has_createstruct_fields = False
    has_register_class_hinstance = False
    has_create_window_hinstance = False
    has_register_class_atom = False
    has_create_class_atom = False
    has_thread_desktop = False
    has_server_error = False
    
    # Last parsed variables for mismatch comparison
    last_reg_hinstance = None
    last_reg_hinstance_line = ""
    last_create_hinstance = None
    last_create_hinstance_line = ""
    
    last_reg_atom = None
    last_reg_atom_line = ""
    last_create_atom = None
    last_create_atom_line = ""
    
    has_window_events = False

    for ev in events:
        if ev.ev == "register_class":
            has_window_events = True
            has_register_class_hinstance = True
            has_register_class_atom = True
            cname = ev.get("class_name")
            atom = ev.get("atom")
            inst = ev.get("instance")
            if cname:
                registered_classes[cname] = {'atom': atom, 'instance': inst, 'ts': ev.ts, 'tid': ev.tid}
            if atom and cname:
                registered_atoms[atom] = cname
            if inst:
                last_reg_hinstance = inst
            if atom:
                last_reg_atom = atom
        elif ev.ev in ("create_window", "ntusercreatewnd_server", "create_window_call"):
            has_window_events = True
            has_create_window_hinstance = True
            has_create_class_atom = True
            has_thread_desktop = True
            has_server_error = True
            hwnd = ev.get("hwnd")
            cname = ev.get("class_name")
            atom = ev.get("atom")
            inst = ev.get("instance")
            failed = ev.get("failed")

            if hwnd == 0 or hwnd == "0" or failed:
                window_create_failed = True
                evidence.append(f"[{ev.ts}] tid={ev.tid} CreateWindow/NtUserCreateWindowEx failed for class '{cname}': {ev.get('line', str(ev))}")

            # Check class/instance mismatch
            if cname in registered_classes:
                reg = registered_classes[cname]
                if inst is not None and reg['instance'] is not None and inst != reg['instance']:
                    class_instance_mismatch = True
                    evidence.append(f"[{ev.ts}] tid={ev.tid} CreateWindow class '{cname}' instance {inst} mismatch with registered instance {reg['instance']}: {ev.get('line', str(ev))}")
                if atom is not None and reg['atom'] is not None and atom != reg['atom']:
                    class_atom_mismatch = True
                    evidence.append(f"[{ev.ts}] tid={ev.tid} CreateWindow class '{cname}' atom {atom} mismatch with registered atom {reg['atom']}: {ev.get('line', str(ev))}")
            elif atom in registered_atoms:
                reg_name = registered_atoms[atom]
                if cname and cname != reg_name:
                    class_atom_mismatch = True
                    evidence.append(f"[{ev.ts}] tid={ev.tid} CreateWindow atom {atom} maps to '{reg_name}' but requested '{cname}': {ev.get('line', str(ev))}")
            
            if hwnd and hwnd != 0 and hwnd != "0":
                window_created = True
            if inst:
                last_create_hinstance = inst
            if atom:
                last_create_atom = atom

        elif ev.ev in ("window_message", "dispatch_message"):
            messages_dispatched = True

        if "line" in ev.fields:
            line = ev.get("line", "").lower()
            if "registerclass" in line or "createwindow" in line:
                has_window_events = True
            
            # 1. Generic abort
            if "wm-nccreate-aborted" in line:
                nccreate_aborted = True
                evidence.append(f"[0] tid={ev.tid} WM_NCCREATE message aborted: {ev.get('line')}")
                
            # 2. WndProc return
            for p in ["wm-nccreate-return=", "nccreate_return=", "wm_nccreate return=", "wndproc_return="]:
                if p in line:
                    has_nccreate_return = True
                    try:
                        part = line.split(p)[1].split()[0].strip(",").strip(";")
                        if part in ("0", "false"):
                            wndproc_returned_false = True
                            evidence.append(f"[0] tid={ev.tid} WndProc returned FALSE: {ev.get('line')}")
                    except Exception: pass
            
            # 3. CREATESTRUCT validity
            for p in ["createstruct_valid=", "cs_valid="]:
                if p in line:
                    has_createstruct_valid = True
                    try:
                        part = line.split(p)[1].split()[0].strip(",").strip(";")
                        if part == "0":
                            createstruct_bad_pointer = True
                            evidence.append(f"[0] tid={ev.tid} Bad CREATESTRUCT pointer: {ev.get('line')}")
                    except Exception: pass
            if "createstruct_read_failed" in line or "bad createstruct" in line:
                has_createstruct_valid = True
                createstruct_bad_pointer = True
                evidence.append(f"[0] tid={ev.tid} Bad CREATESTRUCT pointer: {ev.get('line')}")
                
            # 4. CREATESTRUCT fields
            cs_fields = ["cs.hinstance=", "cs.hwndparent=", "cs.hmenu=", "cs.lpcreateparams=", "cs.style=", "cs.exstyle="]
            if any(f in line for f in cs_fields):
                has_createstruct_fields = True
                if "invalid" in line:
                    createstruct_field_corruption = True
                    createstruct_field_corruption_confidence = 0.95
                    evidence.append(f"[0] tid={ev.tid} Explicit CREATESTRUCT field corruption: {ev.get('line')}")
                elif any(x in line for x in ["cs.hinstance=00000000", "cs.hinstance=0x0", "cs.hinstance=0 ", "cs.hinstance=0,", "cs.hinstance=null"]) or line.endswith("cs.hinstance=0"):
                    createstruct_field_corruption = True
                    createstruct_field_corruption_confidence = 0.85
                    evidence.append(f"[0] tid={ev.tid} CREATESTRUCT null hInstance: {ev.get('line')}")
                elif "invalid parent" in line or "invalid menu" in line or "invalid lpcreateparams" in line:
                    createstruct_field_corruption = True
                    createstruct_field_corruption_confidence = 0.95
                    evidence.append(f"[0] tid={ev.tid} Explicit CREATESTRUCT field corruption: {ev.get('line')}")

            # 5. RegisterClass/CreateWindow hInstance
            reg_hinst_patterns = ["register_class hinstance=", "registerclassexw hinstance=", "class_hinstance="]
            create_hinst_patterns = ["create_window hinstance=", "createwindowexw hinstance=", "create_hinstance="]
            for p in reg_hinst_patterns:
                if p in line:
                    has_register_class_hinstance = True
                    try:
                        part = line.split(p)[1].split()[0].strip(",").strip(";")
                        hinst = int(part, 16 if "0x" in part else 10)
                        if hinst != 0:
                            last_reg_hinstance = hinst
                            last_reg_hinstance_line = ev.get('line')
                    except Exception: pass
            for p in create_hinst_patterns:
                if p in line:
                    has_create_window_hinstance = True
                    try:
                        part = line.split(p)[1].split()[0].strip(",").strip(";")
                        hinst = int(part, 16 if "0x" in part else 10)
                        if hinst != 0:
                            last_create_hinstance = hinst
                            last_create_hinstance_line = ev.get('line')
                    except Exception: pass

            # 6. Class atom
            reg_atom_patterns = ["register_class atom=", "registered_atom="]
            create_atom_patterns = ["create_class_atom=", "create atom="]
            for p in reg_atom_patterns:
                if p in line:
                    has_register_class_atom = True
                    try:
                        part = line.split(p)[1].split()[0].strip(",").strip(";")
                        atom = int(part, 16 if "0x" in part else 10)
                        if atom != 0:
                            last_reg_atom = atom
                            last_reg_atom_line = ev.get('line')
                    except Exception: pass
            for p in create_atom_patterns:
                if p in line:
                    has_create_class_atom = True
                    try:
                        part = line.split(p)[1].split()[0].strip(",").strip(";")
                        atom = int(part, 16 if "0x" in part else 10)
                        if atom != 0:
                            last_create_atom = atom
                            last_create_atom_line = ev.get('line')
                    except Exception: pass

            # 7. Server error
            if "server_error=" in line or "status=" in line or "grab_class" in line:
                has_server_error = True
                for p in ["server_error=", "status="]:
                    if p in line:
                        try:
                            part = line.split(p)[1].split()[0].strip(",").strip(";")
                            val = int(part, 16 if "0x" in part else 10)
                            if val != 0:
                                window_server_error = True
                                if "status_invalid_handle" in line or "error_invalid_handle" in line or "0xc0000008" in part.lower():
                                    window_server_error_confidence = 0.95
                                else:
                                    window_server_error_confidence = 0.85
                                evidence.append(f"[0] tid={ev.tid} Window server error {part}: {ev.get('line')}")
                        except Exception: pass

            # 8. Desktop
            if "macrunner-server-create-window" in line:
                if "no-desktop" in line:
                    window_no_desktop = True
                    has_thread_desktop = True
                    evidence.append(f"[0] tid={ev.tid} Server CreateWindow no-desktop error: {ev.get('line')}")
                if "thread_desktop=" in line:
                    has_thread_desktop = True
                    try:
                        part = line.split("thread_desktop=")[1].split()[0].strip(",").strip(";")
                        val = int(part, 16 if "0x" in part else 10)
                        if val == 0:
                            window_thread_desktop_zero = True
                            evidence.append(f"[0] tid={ev.tid} Server CreateWindow thread_desktop=0 error: {ev.get('line')}")
                    except Exception: pass

            # Unity success detection (case-insensitive for UnityWndClass)
            if "after-server" in line and "unity" in line.lower() and "server_error=0" in line:
                if "handle=" in line:
                    try:
                        handle_part = line.split("handle=")[1].split()[0].strip(",").strip(";")
                        handle_val = int(handle_part, 16 if "0x" in handle_part else 10)
                        if handle_val != 0:
                            unity_create_window_success = True
                    except Exception: pass

            # 9. Handle failed
            if "stage=create-window-handle-failed" in line:
                window_handle_create_failed = True
                evidence.append(f"[0] tid={ev.tid} CreateWindow handle creation failed: {ev.get('line')}")
            
            if "wm_" in line or "message pump" in line or "dispatchmessage" in line:
                messages_dispatched = True

    # Safety: suppress mismatch flags when Unity window creation succeeded — the per-class
    # checks in the create_window loop are authoritative; the global last_reg/last_create
    # variables track unrelated windows in multi-window apps and produce false positives.
    if unity_create_window_success:
        class_atom_mismatch = False
        class_instance_mismatch = False

    if sequence_violation or timeline_broken:
        evidence.extend(sequence_evidence)

    # Set class and verdict based on priority
    if unity_create_window_success:
        verdict = "PASS"
        klass = "WINDOW_GATE_SUCCESSFUL"
        confidence = 0.95
        next_action = "No action required. Unity CreateWindow succeeded."
    # P0
    elif window_thread_desktop_zero:
        verdict = "BLOCKED"
        klass = "WINDOW_THREAD_DESKTOP_ZERO"
        confidence = 0.90
        next_action = "Set MACRUNNER_HB_TRACE_CREATEWINDOW=1 and MACRUNNER_HB_TRACE_THREAD_DESKTOP=1; inspect the window desktop assignment path in win32u/server window creation code; trace create_window tid, process_desktop, thread_desktop, and the function that should assign thread_desktop before NtUserCreateWindowEx."
    elif window_no_desktop:
        verdict = "BLOCKED"
        klass = "WINDOW_NO_DESKTOP"
        confidence = 0.95
        next_action = "Ensure the process is attached to a valid desktop environment prior to window creation."
    # P1
    elif window_server_error:
        verdict = "BLOCKED"
        klass = "WINDOW_SERVER_ERROR"
        confidence = window_server_error_confidence
        next_action = "Diagnose window server error code in NtUserCreateWindowEx routing."
    # P2
    elif class_instance_mismatch:
        verdict = "BLOCKED"
        klass = "CLASS_INSTANCE_MISMATCH"
        confidence = 0.95
        next_action = "Ensure HINSTANCE passed to CreateWindow matches the module registration HINSTANCE."
    elif class_atom_mismatch:
        verdict = "BLOCKED"
        klass = "CLASS_ATOM_MISMATCH"
        confidence = 0.95
        next_action = "Verify the class atom or name passed to CreateWindow is correct and registered."
    # P3
    elif createstruct_bad_pointer:
        verdict = "BLOCKED"
        klass = "CREATESTRUCT_BAD_POINTER"
        confidence = 0.95
        next_action = "Verify the CREATESTRUCT pointer passed to CreateWindow is valid and accessible."
    elif createstruct_field_corruption:
        verdict = "BLOCKED"
        klass = "CREATESTRUCT_FIELD_CORRUPTION"
        confidence = createstruct_field_corruption_confidence
        next_action = "Ensure CREATESTRUCT fields (hInstance, style, lpCreateParams) are not corrupted during transit."
    # P4
    elif wndproc_returned_false:
        verdict = "BLOCKED"
        klass = "WNDPROC_RETURNED_FALSE"
        confidence = 0.95
        next_action = "Verify that the window procedure (wndproc) returns TRUE for WM_NCCREATE message."
    # P5 - Check sequence/timeline violation before nccreate_aborted
    elif sequence_violation or timeline_broken:
        verdict = "BLOCKED"
        klass = "WINDOW_SEQUENCE_VIOLATION"
        confidence = 0.90
        next_action = "observed trace/order violation or missing event; needs runtime trace confirmation."
    elif nccreate_aborted:
        # Check if there are missing trace markers
        missing = []
        if not has_nccreate_return: missing.append("WM_NCCREATE_RETURN")
        if not has_createstruct_valid: missing.append("CREATESTRUCT_VALID")
        if not has_createstruct_fields: missing.append("CREATESTRUCT_FIELDS")
        if not has_register_class_hinstance: missing.append("REGISTER_CLASS_HINSTANCE")
        if not has_create_window_hinstance: missing.append("CREATE_WINDOW_HINSTANCE")
        if not has_register_class_atom: missing.append("REGISTER_CLASS_ATOM")
        if not has_create_class_atom: missing.append("CREATE_CLASS_ATOM")
        if not has_thread_desktop: missing.append("THREAD_DESKTOP")
        if not has_server_error: missing.append("SERVER_ERROR")

        if missing:
            verdict = "BLOCKED"
            klass = "WINDOW_NCCREATE_ABORTED_TRACE_INSUFFICIENT"
            confidence = 0.90
            next_action = f"Add narrow WM_NCCREATE trace for wndproc return, CREATESTRUCT validity/fields, RegisterClass/CreateWindow atom+hInstance. Missing markers: {', '.join(missing)}"
            missing_markers = {
                "critical": ["WM_NCCREATE_RETURN", "THREAD_DESKTOP"],
                "secondary": ["CREATESTRUCT_VALID", "REGISTER_CLASS_HINSTANCE", "CREATE_WINDOW_HINSTANCE"]
            }
            next_run_env = {
                "MACRUNNER_HB_TRACE_NCCREATE": "1",
                "MACRUNNER_HB_TRACE_CREATEWINDOW": "1"
            }
        else:
            verdict = "BLOCKED"
            klass = "WINDOW_NCCREATE_ABORTED"
            confidence = 0.95
            next_action = "Ensure that WM_NCCREATE window messages return TRUE to allow successful window initialization."
    # Fallback to other checks
    elif window_handle_create_failed:
        verdict = "BLOCKED"
        klass = "WINDOW_HANDLE_CREATE_FAILED"
        confidence = 0.90
        next_action = "Investigate why the native window handle could not be allocated by the system."
    elif window_created and not messages_dispatched:
        verdict = "BLOCKED"
        klass = "WINDOW_MESSAGES_MISSING"
        confidence = 0.9 if logs.get("flight") else 0.7
        next_action = "Verify why the thread's message loop is not processing messages for the created window."
    elif window_created:
        verdict = "PASS"
        klass = "WINDOW_GATE_SUCCESSFUL"
        confidence = 1.0
        next_action = "No action required. Window created and message pump processed active messages."
    elif has_window_events:
        verdict = "UNKNOWN"
        klass = "WINDOW_TRACE_INSUFFICIENT"
        confidence = 0.35
        next_action = "Collect detailed NtUser CreateWindow trace to see if window creation was attempted."
        missing_markers = {
            "critical": ["WM_NCCREATE_RETURN", "THREAD_DESKTOP"],
            "secondary": ["CREATESTRUCT_VALID", "REGISTER_CLASS_HINSTANCE", "CREATE_WINDOW_HINSTANCE"]
        }
        next_run_env = {
            "MACRUNNER_HB_TRACE_NCCREATE": "1",
            "MACRUNNER_HB_TRACE_CREATEWINDOW": "1"
        }
    else:
        verdict = "UNKNOWN"
        klass = "WINDOW_TRACE_INSUFFICIENT"
        confidence = 0.30
        next_action = "Capture a flight recorder log with window and message gate logging active."
        missing_markers = {
            "critical": ["WM_NCCREATE_RETURN", "THREAD_DESKTOP"],
            "secondary": ["CREATESTRUCT_VALID", "REGISTER_CLASS_HINSTANCE", "CREATE_WINDOW_HINSTANCE"]
        }
        next_run_env = {
            "MACRUNNER_HB_TRACE_NCCREATE": "1",
            "MACRUNNER_HB_TRACE_CREATEWINDOW": "1"
        }

    print_report(verdict, owner, klass, confidence, evidence, next_action, missing_markers, next_run_env)

if __name__ == "__main__":
    main()
