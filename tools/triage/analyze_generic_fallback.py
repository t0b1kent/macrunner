import sys
import os

sys.path.append(os.path.dirname(os.path.abspath(__file__)))
from triage_common import find_logs, stream_events, print_report

# Routing table: (patterns, owner, class, confidence, next_action)
_RULES = [
    (
        ["virtual_setup_exception", "redirect loop", "macrunner-hb-setup-raise: redirect loop"],
        "Lane A",
        "TRANSLATOR_STACK_OVERFLOW",
        0.80,
        "HyperBridge translator stack exhaustion at exception dispatch (not a guest fault). "
        "The native bridge/exception stack overflows during translation/exception delivery. "
        "Check the run_x64_depth guard + the per-block bridge-stack preventive check (macrunner_hb.c) "
        "and the redirect target / re-entrancy guard in setup_raise_exception (signal_arm64.c). "
        "See reports/research/LANE-A-OVERFLOW-ROOT-FIX-BRIEF.md.",
    ),
    (
        ["c0000026", "status_invalid_disposition", "invalid_disposition",
         "macrunner-hb-seh-host-boundary"],
        "Lane A",
        "GENERIC_INVALID_DISPOSITION_SEH",
        0.55,
        "SEH unwind hit a host-call thunk with no PE unwind metadata. "
        "Map pc to module/RVA; add RtlInstallFunctionTableCallback for the thunk region "
        "or synthesize the unwind in virtual_unwind (signal_arm64.c). "
        "Enable MACRUNNER_HB_TRACE_SEH=1 for the full detail line.",
    ),
    (
        ["c000001d", "illegal instruction", "unsupported opcode", "ud2",
         "unimplemented opcode", "bad instruction"],
        "Lane B",
        "GENERIC_ILLEGAL_INSTRUCTION",
        0.55,
        "HyperBridge decoder/lifter/interpreter missing CPU instruction support. "
        "Identify the faulting opcode (capstone decode at pc) and add it to the JIT. "
        "Enable MACRUNNER_HB_TRACE_DECODE=1.",
    ),
    (
        ["c0000005", "access violation", "page_fault", "segmentation fault",
         "invalid memory address", "bad memory access"],
        "Lane C",
        "GENERIC_ACCESS_VIOLATION",
        0.50,
        "Memory access violation — likely VirtualAlloc/NtProtect mapping gap or "
        "bad pointer from guest. Enable MACRUNNER_HB_TRACE_MEMORY=1 and check "
        "the faulting address against the guest page table.",
    ),
    (
        ["assertion failed", "assert(", "assert failed", "failed check",
         "check failed", "debug assertion"],
        "Lane A",
        "GENERIC_ASSERTION_FAILED",
        0.55,
        "Internal engine state invariant violated. Identify the assert site and "
        "the violated invariant. Check HyperBridge JIT/core state at that point.",
    ),
    (
        ["vulkan", "metal", "wined3d", "gl_invalid", "mtldevice", "mtlcommand",
         "gfxdevice", "d3d11", "dxgi", "winemetal"],
        "Lane D",
        "GENERIC_GRAPHICS_FAULT",
        0.45,
        "Graphics API translation or pipeline issue. Identify the failing Metal/D3D11 "
        "call and check the DXMT/winemetal translation layer. "
        "Enable MACRUNNER_HB_TRACE_D3D=1.",
    ),
    (
        ["wow64", "xtajit", "btcpu", "btcpuprocessinit", "wow64cpu",
         "macrunner-wow64", "macrunner-xtajit"],
        "Lane A",
        "GENERIC_WOW64_FAULT",
        0.50,
        "WOW64 subsystem initialisation or thunking issue. Check that "
        "wow64cpu.dll/xtajit.dll are deployed to syswow64 in the prefix. "
        "Enable MACRUNNER_HB_TRACE_WOW64=1.",
    ),
]


def _collect_text(logs):
    # Read HEAD + TAIL of each log: early markers (faults, init) live near the top, but late
    # markers (class registration, CreateWindow, overflow, process exit) live near the end of a
    # 30MB+ run.log. Reading only the head mislabels late-stage exits. Binary read to seek cleanly.
    parts = []
    CHUNK = 512 * 1024
    for key in ("run", "stderr", "stdout"):
        path = logs.get(key)
        if path and os.path.exists(path):
            try:
                size = os.path.getsize(path)
                with open(path, "rb") as f:
                    parts.append(f.read(CHUNK).decode("utf-8", "ignore"))
                    if size > 2 * CHUNK:
                        f.seek(size - CHUNK)
                        parts.append(f.read().decode("utf-8", "ignore"))
            except Exception:
                pass
    return "\n".join(parts)


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 analyze_generic_fallback.py <run-dir | run.log>")
        sys.exit(1)

    path = sys.argv[1]
    logs = find_logs(path)
    events = list(stream_events(logs))

    verdict = "UNKNOWN"
    owner = "Unknown"
    klass = "GENERIC_UNCLASSIFIED"
    confidence = 0.20
    evidence = []
    next_action = "No specific exception pattern matched. Collect more trace data."

    text = _collect_text(logs).lower()
    if not text and not events:
        print_report(verdict, owner, klass, 0.0, evidence, next_action)
        return

    # Also build text from raw event lines (covers flight.jsonl mode)
    for ev in events:
        line = ev.get("line", "")
        if line:
            text += "\n" + line.lower()

    # i386 actually executing = NOT a WOW64 init/thunk fault. The presence of wow64/xtajit/btcpu
    # keywords in a run where the CPU is running is normal, not a failure. Don't fire the generic
    # WOW64 fault then; defer the verdict to the PE32 handoff analyzer.
    i386_executing = ("cpu_simulate" in text or "btcpusimulate" in text) and (
        "eip=" in text or ("jit" in text and "block" in text))

    # A graphics keyword (winemetal/dxgi/d3d11) in the log usually comes from module LOAD/SYNC,
    # not an actual graphics API call. Only treat it as a graphics fault when a real call/device
    # path was reached — otherwise it mislabels pre-window exits as GENERIC_GRAPHICS_FAULT.
    graphics_call_reached = any(p in text for p in (
        "d3d11createdevice", "createdxgifactory", "createmetalview", "mtldevice",
        "d3d11_swapchain", "createswapchain", "winemetal[hwnd]", "mtlcreate"))

    # Process reached window-CLASS registration but exited before any CreateWindowEx attempt.
    # This is a window/COM/process-init exit, NOT a graphics fault (the most common HK mislabel).
    reached_classreg = ("server-create-class" in text or "create-class" in text
                        or "register_class" in text or "registerclass" in text)
    window_attempted = any(p in text for p in (
        "createwindowex", "ntusercreatewindow", "create_window", "create-window-handle",
        "wm-nccreate", "wndproc"))
    proc_init_done = "wine-process-primary-installed" in text or "wine-process-start" in text
    reached_classreg_no_window = reached_classreg and proc_init_done and not window_attempted

    matched_rule = None
    for patterns, rule_owner, rule_class, rule_conf, rule_action in _RULES:
        if rule_class == "GENERIC_WOW64_FAULT" and (i386_executing or reached_classreg_no_window):
            continue  # CPU running OR process reached class-reg cleanly — wow64 keywords are init noise, not a fault
        if rule_class == "GENERIC_GRAPHICS_FAULT" and not graphics_call_reached:
            continue  # graphics keyword from module load/sync only — not an actual graphics fault
        hits = [p for p in patterns if p in text]
        if hits:
            matched_rule = (rule_owner, rule_class, rule_conf, rule_action, hits)
            break

    if matched_rule:
        owner, klass, confidence, next_action, hits = matched_rule
        verdict = "BLOCKED"
        # Find an evidence line for each hit pattern
        for hit in hits[:3]:
            for line in text.splitlines():
                if hit in line and line.strip():
                    evidence.append(line.strip()[:200])
                    break
    elif reached_classreg_no_window:
        verdict = "BLOCKED"
        owner = "Lane D"
        klass = "HK_WINDOW_INIT_EXIT"
        confidence = 0.70
        next_action = ("Process registered window classes but EXITED before any CreateWindowEx attempt — "
                       "this is a window/COM/process-init exit, NOT a graphics fault (no D3D11/Metal call "
                       "reached). Find why the process exits after class registration: services.exe/rpcss "
                       "startup, warm/registered prefix, or COM/loader init before window creation. "
                       "Run with WINEDEBUG=+module,+process and MACRUNNER_HB_TRACE_CREATEWINDOW=1.")
        for line in text.splitlines():
            if "server-create-class" in line and line.strip():
                evidence.append(line.strip()[:200])
                break
    elif i386_executing:
        verdict = "PASS"
        owner = "Lane A"
        klass = "PE32_I386_EXECUTING"
        confidence = 0.85
        next_action = ("i386/WOW64 CPU is executing guest code; no generic fault detected. Defer to the "
                       "PE32 handoff analyzer; profile where i386 execution stalls + cover ISA gaps.")

    print_report(verdict, owner, klass, confidence, evidence, next_action)


if __name__ == "__main__":
    main()
