import sys
import os

sys.path.append(os.path.dirname(os.path.abspath(__file__)))
from triage_common import find_logs, stream_events, print_report

# Routing table: (patterns, owner, class, confidence, next_action)
_RULES = [
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
    parts = []
    for key in ("run", "stderr", "stdout"):
        path = logs.get(key)
        if path and os.path.exists(path):
            try:
                with open(path, encoding="utf-8", errors="ignore") as f:
                    parts.append(f.read(512 * 1024))
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

    matched_rule = None
    for patterns, rule_owner, rule_class, rule_conf, rule_action in _RULES:
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

    print_report(verdict, owner, klass, confidence, evidence, next_action)


if __name__ == "__main__":
    main()
