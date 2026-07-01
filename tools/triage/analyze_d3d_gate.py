import sys
import os

# Ensure tools/triage is in path so we can import triage_common
sys.path.append(os.path.dirname(os.path.abspath(__file__)))
from triage_common import find_logs, stream_events, print_report

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 analyze_d3d_gate.py <flight.jsonl | run-dir | run.log>")
        sys.exit(1)

    path = sys.argv[1]
    logs = find_logs(path)
    events = list(stream_events(logs))

    verdict = "UNKNOWN"
    owner = "Lane C"
    klass = "D3D_TRACE_INSUFFICIENT"
    confidence = 0.5
    evidence = []
    next_action = "Capture a flight recorder log with D3D and graphics gate logging enabled."
    max_evidence = 32

    def add_evidence(line):
        if len(evidence) < max_evidence:
            evidence.append(line)

    if not events:
        print_report(verdict, "Unknown", klass, 0.0, evidence, next_action)
        return

    # Check for D3D pipeline events
    d3d11_loaded = False
    dxgi_loaded = False
    wined3d_loaded = False
    create_dxgi_factory_seen = False
    d3d11_create_device_seen = False
    present_seen = False
    has_d3d_events = False
    dxgi_iat_count = 0
    real_factory_count = 0
    d3d11_iat_count = 0
    d3d11_device_marker_count = 0
    swapchain_count = 0
    present_count = 0

    for ev in events:
        if ev.ev == "module_map":
            name = str(ev.get("name", "")).lower()
            if "d3d11" in name:
                d3d11_loaded = True
                has_d3d_events = True
                add_evidence(f"[{ev.ts}] tid={ev.tid} module_map d3d11.dll loaded at {ev.get('base_addr')}")
            elif "dxgi" in name:
                dxgi_loaded = True
                has_d3d_events = True
                add_evidence(f"[{ev.ts}] tid={ev.tid} module_map dxgi.dll loaded at {ev.get('base_addr')}")
            elif "wined3d" in name:
                wined3d_loaded = True
                has_d3d_events = True
                add_evidence(f"[{ev.ts}] tid={ev.tid} module_map wined3d.dll loaded at {ev.get('base_addr')}")
        elif ev.ev == "d3d_entry":
            has_d3d_events = True
            api = ev.get("api", "")
            add_evidence(f"[{ev.ts}] tid={ev.tid} d3d_entry api={api}")
            if "CreateDXGIFactory" in api:
                create_dxgi_factory_seen = True
            elif "D3D11CreateDevice" in api:
                d3d11_create_device_seen = True
            elif "Present" in api:
                present_seen = True
        elif ev.ev == "log_line":
            raw_line = ev.get("line", "")
            line = raw_line.lower()
            if "d3d11.dll" in line or "d3d11" in line:
                d3d11_loaded = True
                has_d3d_events = True
            if "dxgi.dll" in line or "dxgi" in line:
                dxgi_loaded = True
                has_d3d_events = True
            if "wined3d" in line:
                wined3d_loaded = True
                has_d3d_events = True
            is_iat = "macrunner-hb-iat" in line
            real_factory = "macrunner-hb-d3d-boundary" in line and "createdxgifactory" in line
            if real_factory:
                create_dxgi_factory_seen = True
                has_d3d_events = True
                real_factory_count += 1
                add_evidence(f"real DXGI factory boundary: {raw_line}")
            elif "createdxgifactory" in line and is_iat:
                dxgi_iat_count += 1
            real_d3d11_boundary = "macrunner-hb-d3d-boundary" in line and "d3d11createdevice" in line
            dxmt_device_marker = (
                "macrunner-dxmt-fence: device createfence" in line or
                "maximum supported feature level" in line
            )
            if real_d3d11_boundary or dxmt_device_marker:
                d3d11_create_device_seen = True
                has_d3d_events = True
                d3d11_device_marker_count += 1
                add_evidence(f"real D3D11 device marker: {raw_line}")
            elif "d3d11createdevice" in line and is_iat:
                d3d11_iat_count += 1
            if "createswapchain" in line or "macrunner-hb-dxgi-swapchain" in line:
                swapchain_count += 1
                has_d3d_events = True
                add_evidence(f"DXGI swapchain marker: {raw_line}")
            if "present" in line and ("d3d" in line or "dxgi" in line or "swapchain" in line or "frame" in line):
                present_seen = True
                has_d3d_events = True
                present_count += 1
                add_evidence(f"DXGI present marker: {raw_line}")

    # GfxDevice SEH boundary blocker check
    gfxdevice_seh_blocker = False
    gfx_idx = -1
    seh_idx = -1
    d3d_seen_after_seh = False
    pc_value = "0x10B44F015"

    for idx, ev in enumerate(events):
        line = ev.get("line", "")
        if not line:
            continue
        line_lower = line.lower()
        if "gfxdevice: creating device client" in line_lower or "gfxdevice: creating device" in line_lower:
            gfx_idx = idx
        elif "macrunner-hb-seh-host-boundary" in line_lower:
            seh_idx = idx
            if "pc=" in line_lower:
                try:
                    pc_value = line.split("pc=")[1].split()[0].strip(",").strip(";")
                except Exception: pass
        elif "d3d11createdevice" in line_lower or "d3d11_create_device" in line_lower:
            if seh_idx != -1 and idx > seh_idx:
                d3d_seen_after_seh = True

    if gfx_idx != -1 and seh_idx != -1 and not d3d_seen_after_seh:
        gfxdevice_seh_blocker = True

    if has_d3d_events:
        evidence.insert(
            0,
            "raw D3D gate counts: "
            f"dxgi_iat={dxgi_iat_count} real_factory={real_factory_count} "
            f"d3d11_iat={d3d11_iat_count} "
            f"d3d11_device_markers={d3d11_device_marker_count} "
            f"swapchain={swapchain_count} present={present_count}"
        )

    missing_markers = None
    next_run_env = None

    if gfxdevice_seh_blocker:
        verdict = "BLOCKED"
        owner = "Lane A"
        klass = "GFXDEVICE_SEH_HOST_BOUNDARY_BEFORE_D3D11"
        confidence = 0.95
        next_action = f"Resolve/trace macrunner-hb-seh-host-boundary pc={pc_value} before D3D11CreateDevice; map pc to module/RVA and classify SEH/ARM64EC boundary."
        evidence.append(f"SEH Host boundary hit: {events[seh_idx].get('line')}")
    elif not (d3d11_loaded or dxgi_loaded or wined3d_loaded or create_dxgi_factory_seen or d3d11_create_device_seen or present_seen):
        verdict = "UNKNOWN"
        klass = "D3D_TRACE_INSUFFICIENT"
        confidence = 0.30
        next_action = "Capture a flight recorder log with D3D and graphics gate logging enabled."
        missing_markers = {
            "critical": ["D3D_ENTRY", "MODULE_MAP"]
        }
        next_run_env = {
            "MACRUNNER_HB_TRACE_D3D": "1"
        }
    elif wined3d_loaded:
        verdict = "BLOCKED"
        klass = "WINED3D_FALLBACK"
        confidence = 1.0 if logs.get("flight") else 0.8
        next_action = "Ensure the Metal/DXMT backend is selected instead of falling back to WineD3D."
    elif not d3d11_loaded:
        verdict = "BLOCKED"
        klass = "D3D11_DLL_NOT_LOADED"
        confidence = 1.0 if logs.get("flight") else 0.8
        next_action = "Ensure that d3d11.dll is packaged correctly and present in the application's search path."
    elif not dxgi_loaded:
        verdict = "BLOCKED"
        klass = "DXGI_DLL_NOT_LOADED"
        confidence = 1.0 if logs.get("flight") else 0.8
        next_action = "Check if dxgi.dll was loaded or check for load-time dependencies in the log."
    elif not create_dxgi_factory_seen:
        verdict = "BLOCKED"
        klass = "CREATE_DXGI_FACTORY_MISSING"
        confidence = 1.0 if logs.get("flight") else 0.8
        next_action = "Investigate why the application did not invoke CreateDXGIFactory to initialize DXGI."
    elif not d3d11_create_device_seen:
        verdict = "BLOCKED"
        klass = "D3D11_CREATE_DEVICE_MISSING"
        confidence = 1.0 if logs.get("flight") else 0.8
        next_action = "Verify why the application did not call D3D11CreateDevice or check if creation returned an error."
    elif not present_seen:
        verdict = "BLOCKED"
        klass = "PRESENT_MISSING"
        confidence = 0.9 if logs.get("flight") else 0.7
        next_action = "Determine why the application failed to swap chains or call Present to render frames."
    elif has_d3d_events:
        verdict = "PASS"
        klass = "D3D_GATE_SUCCESSFUL"
        confidence = 1.0
        next_action = "No action required. Direct3D rendering pipeline completed successfully."
    else:
        verdict = "UNKNOWN"
        klass = "D3D_TRACE_INSUFFICIENT"
        confidence = 0.30
        next_action = "Capture a flight recorder log with D3D and graphics gate logging enabled."
        missing_markers = {
            "critical": ["D3D_ENTRY", "MODULE_MAP"]
        }
        next_run_env = {
            "MACRUNNER_HB_TRACE_D3D": "1"
        }

    print_report(verdict, owner, klass, confidence, evidence, next_action, missing_markers, next_run_env)

if __name__ == "__main__":
    main()
