import sys
import os
import re

sys.path.append(os.path.dirname(os.path.abspath(__file__)))
from triage_common import find_logs, print_report

# analyze_dll_load.py — detect blockers that fire BEFORE the graphics pipeline
# can start. These must outrank graphics/window classes because the boot dies at
# DLL load / system-DLL init, so any "no D3D markers" / "window server error"
# verdict produced downstream is a false positive (the run never reached that stage).
#
# Two classes (both precede graphics, hence higher priority than WINED3D_FALLBACK etc.):
#
#   DLL_LOAD_FAILURE
#     A graphics DLL (dxgi.dll / d3d11.dll) fails to load with c0000135 AND the
#     virtual memory manager reports `map_fixed_area out of memory`. That combo
#     means the guest address-space layout is exhausted/mis-mapped (the loader
#     cannot place the DLL at its preferred base), NOT that the Metal/DXMT backend
#     is mis-selected. The d3d gate mis-reads a stray `wined3d.dll` import-prefix
#     line as a WineD3D fallback in this scenario.
#
#   SYSTEM_DLL_INIT_CASCADE
#     >=3 distinct wine helper processes (control.exe / iexplore.exe / svchost /
#     plugplay / winedevice ...) each take c0000005 at the SAME faulting PC while
#     inside `loader_init Initializing system dll for L"<helper>" failed`. A
#     single shared crash PC across many helpers means one system DLL in the dist
#     is corrupted/incompatible (binary-graft class), not a per-app graphics bug.

_GRAPHICS_DLL_RE = re.compile(r'(dxgi|d3d11|d3d10|d3d9|wined3d)\.dll', re.IGNORECASE)
_LDR_FAIL_RE = re.compile(r'macrunner-ldr-(?:load-dll|LdrLoadDll)-fail:\s*status=c0000135', re.IGNORECASE)
_LDR_FAIL_NAME_RE = re.compile(
    r'macrunner-ldr-(?:load-dll|LdrLoadDll)-fail:\s*status=c0000135\s*'
    r'(?:lib|name)=L"([^"]+)"', re.IGNORECASE)
_MAP_FIXED_OOM_RE = re.compile(r'map_fixed_area\s+out of memory', re.IGNORECASE)
_LOADER_INIT_FAIL_RE = re.compile(
    r'loader_init\s+Initializing system dll for L"([^"]+)"\s+failed,\s*status\s*c0000005',
    re.IGNORECASE)
_DISPATCH_AV_RE = re.compile(
    r'dispatch_exception\s+code=c0000005.*?\baddr=([0-9A-Fa-f]+)', re.IGNORECASE)
_OTHER_DISPATCH_RE = re.compile(
    r'dispatch_exception\s+code=(?!c0000005\b)[0-9A-Fa-f]+', re.IGNORECASE)
_INIT_SYSTEM_DLL_RE = re.compile(
    r'loader_init\s+Initializing system dll for L"([^"]+)"', re.IGNORECASE)


def _collect_text(logs):
    parts = []
    for key in ("run", "stderr", "stdout"):
        path = logs.get(key)
        if not path or not os.path.exists(path):
            continue
        try:
            size = os.path.getsize(path)
            with open(path, "r", encoding="utf-8", errors="ignore") as f:
                if size <= 6 * 1024 * 1024:
                    parts.append(f.read())
                else:
                    parts.append(f.read(2 * 1024 * 1024))
                    f.seek(max(0, size - 4 * 1024 * 1024))
                    parts.append("\n" + f.read())
        except Exception:
            continue
    return "\n".join(parts)


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 analyze_dll_load.py <run-dir | run.log | flight.jsonl>")
        sys.exit(1)

    logs = find_logs(sys.argv[1])
    text = _collect_text(logs)
    if not text:
        print_report("UNKNOWN", "Lane C", "DLL_LOAD_TRACE_INSUFFICIENT", 0.0, [],
                     "No readable run/stderr/stdout log found.")
        return

    lines = text.splitlines()
    evidence = []
    max_ev = 16

    def add_ev(s):
        if len(evidence) < max_ev:
            evidence.append(s)

    # --- DLL_LOAD_FAILURE -------------------------------------------------
    graphics_load_fail_lines = []
    any_ldr_c0000135 = False
    for ln in lines:
        if _LDR_FAIL_RE.search(ln):
            any_ldr_c0000135 = True
            m = _LDR_FAIL_NAME_RE.search(ln)
            if m and _GRAPHICS_DLL_RE.search(m.group(1)):
                graphics_load_fail_lines.append(ln.strip())

    map_fixed_oom_lines = [ln.strip() for ln in lines if _MAP_FIXED_OOM_RE.search(ln)]
    loader_init_c0000135_lines = [
        ln.strip() for ln in lines
        if 'loader_init' in ln.lower() and 'c0000135' in ln.lower()
    ]

    dll_load_failure = bool(graphics_load_fail_lines) and bool(map_fixed_oom_lines)

    # --- SYSTEM_DLL_INIT_CASCADE -----------------------------------------
    # Walk lines in order; track the most recent dispatch_exception c0000005 PC
    # per thread, then attribute the next `loader_init Initializing system dll`
    # failure on the same thread to that PC. >=3 distinct helpers sharing one PC
    # => the shared system DLL is broken.
    pc_helper_pairs = []  # (pc, helper)
    cur_pc = None
    for ln in lines:
        m_disp = _DISPATCH_AV_RE.search(ln)
        if m_disp:
            cur_pc = m_disp.group(1).upper()
            continue
        m_init_fail = _LOADER_INIT_FAIL_RE.search(ln)
        if m_init_fail and cur_pc:
            pc_helper_pairs.append((cur_pc, m_init_fail.group(1)))
            cur_pc = None  # attribute one PC per init-fail (the immediately preceding fault)
            continue
        # A different exception code (not access-violation) starts a new event; reset.
        # NOTE: dispatch_exception is printed across many continuation lines
        # (info[0]=, pc=, x0=, ...) that contain "dispatch_exception" but no code=,
        # so we must NOT reset on those — only reset on a dispatch_exception with
        # a code= that is not c0000005.
        if _OTHER_DISPATCH_RE.search(ln):
            cur_pc = None

    # Tally helpers per PC
    pc_to_helpers = {}
    for pc, helper in pc_helper_pairs:
        pc_to_helpers.setdefault(pc, set()).add(helper)

    cascade_pc = None
    cascade_helpers = set()
    for pc, helpers in pc_to_helpers.items():
        if len(helpers) >= 3:
            cascade_pc = pc
            cascade_helpers = helpers
            break

    system_dll_init_cascade = cascade_pc is not None

    # --- Verdict ----------------------------------------------------------
    if dll_load_failure:
        add_ev(f"graphics DLL load failure (c0000135) + map_fixed_area OOM:")
        for ln in graphics_load_fail_lines[:3]:
            add_ev("  " + ln)
        for ln in map_fixed_oom_lines[:3]:
            add_ev("  " + ln)
        if loader_init_c0000135_lines:
            add_ev("loader_init c0000135:")
            add_ev("  " + loader_init_c0000135_lines[0][:240])
        print_report(
            "BLOCKED", "Lane A",
            "DLL_LOAD_FAILURE", 0.90,
            evidence,
            "Guest address-space/layout exhaustion: dxgi/d3d11 failed to map "
            "(map_fixed_area out of memory, c0000135) BEFORE the graphics "
            "pipeline started. This is NOT a Metal/DXMT backend-selection issue. "
            "Investigate the loader's fixed-map reservation / preferred-base "
            "collision / address-space layout for the failing DLL. "
            "Enable MACRUNNER_HB_TRACE_LOADER=1 and MACRUNNER_HB_TRACE_MEMORY=1.")
        return

    if system_dll_init_cascade:
        add_ev(f">=3 wine helpers crash at the SAME PC {cascade_pc} during dll-init:")
        for h in sorted(cascade_helpers)[:6]:
            add_ev("  helper: " + h)
        # Pull a couple of real log lines for the shared PC
        shown = 0
        for ln in lines:
            if shown >= 4:
                break
            if cascade_pc.lower() in ln.lower() and 'dispatch_exception' in ln.lower():
                add_ev("  " + ln.strip()[:240])
                shown += 1
        for ln in lines:
            if _LOADER_INIT_FAIL_RE.search(ln):
                add_ev("  " + ln.strip()[:240])
                break
        print_report(
            "BLOCKED", "Lane A",
            "SYSTEM_DLL_INIT_CASCADE", 0.88,
            evidence,
            "A single shared faulting PC across >=3 wine helper processes during "
            "system-DLL init means one system DLL in the dist is corrupted or "
            "ABI-incompatible (binary-graft class). Re-derive/redeploy the offending "
            "system DLL; map the shared PC to its module/RVA. "
            "Enable MACRUNNER_HB_TRACE_LOADER=1 and MACRUNNER_HB_TRACE_SEH=1.")
        return

    # Nothing specific — let the other analyzers decide.
    if any_ldr_c0000135:
        # Some DLL failed to load but not the graphics-address-space pattern;
        # don't claim a class, just note it for context.
        print_report("UNKNOWN", "Lane C", "DLL_LOAD_TRACE_INSUFFICIENT", 0.20,
                     ["non-graphics c0000135 load failure seen, but no map_fixed_area OOM "
                      "and no >=3-helper init cascade — not enough for DLL_LOAD_FAILURE."],
                     "No DLL-load/init blocker matched; defer to other analyzers.")
    else:
        print_report("PASS", "Lane C", "DLL_LOAD_OK", 0.50, [],
                     "No DLL load/init blocker detected.")


if __name__ == "__main__":
    main()
