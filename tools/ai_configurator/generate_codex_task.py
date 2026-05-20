#!/usr/bin/env python3
"""
MacRunner AI Configurator — Codex Next Task Generator

Input:
  - reports/ai-configurator/latest-configurator-result.json

Output:
  - reports/ai-configurator/CODEX-NEXT-TASK.md

Generates a precise, actionable prompt for Codex with:
- exact current blocker
- evidence paths
- root layer
- forbidden wrong fixes
- whether runtime code may be touched
- expected tests
- acceptance criteria
- rollback/cleanup rules
"""

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path


def load_json(path: Path) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def generate_codex_task(result: dict) -> str:
    category = result.get("blocker_category", "UNKNOWN_NEEDS_PROBE")
    blocker = result.get("current_blocker", "Unknown blocker")
    confidence = result.get("confidence", "LOW")
    safe = result.get("safe_to_touch_runtime", False)
    action = result.get("recommended_codex_action", "Collect more evidence.")
    forbidden = result.get("forbidden_actions", [])
    criteria = result.get("acceptance_criteria", [])
    rollback = result.get("rollback_rules", [])
    compat = result.get("compatibility", {})
    visual = result.get("visual", {})
    app_path = result.get("app_profile_path", "unknown")
    run_dir = result.get("selected_run_dir", "unknown")

    lines = [
        "# Codex Next Task",
        "",
        f"**Generated at:** {datetime.now(timezone.utc).isoformat().replace('+00:00', 'Z')}",
        f"**App profile:** {app_path}",
        f"**Run directory:** {run_dir}",
        "",
        "## Current Blocker",
        "",
        f"- **Category:** `{category}`",
        f"- **Confidence:** {confidence}",
        f"- **Description:** {blocker}",
        f"- **Safe to touch runtime:** {'YES' if safe else 'NO'}",
        "",
        "## What to Fix",
        "",
        action,
        "",
        "## What NOT to Fix",
        "",
    ]

    if forbidden:
        for rule in forbidden:
            lines.append(f"- {rule}")
    else:
        lines.append("- (none specified)")
    lines.append("")

    # Category-specific detailed instructions
    lines.append("## Detailed Instructions")
    lines.append("")

    if category == "STALE_BUILD":
        lines.extend([
            "1. Identify which build artifact is stale (ntdll.so, libhyperbridge.a, helper .exe).",
            "2. Remove stale objects: `rm -f engine/hyperbridge/src/*.o engine/hyperbridge/libhyperbridge.a`.",
            "3. Rebuild: `make -j4 libhyperbridge.a`.",
            "4. Force relink: `rm -f dlls/ntdll/ntdll.so; make -j4 dlls/ntdll/ntdll.so install`.",
            "5. Codesign: `codesign --force --sign - ntdll.so`.",
            "6. Recompile helper if `.c` changed.",
            "7. Rerun smoke and verify.",
        ])
    elif category == "WINE_LIFECYCLE":
        lines.extend([
            "1. Verify AF_UNIX socket creation is allowed in the current session/sandbox.",
            "2. Check if `TMPDIR` is writable and not restricted.",
            "3. If sandbox blocks AF_UNIX: run from a host/session with full access.",
            "4. Do NOT patch the app or engine for a sandbox issue.",
            "5. Document the sandbox restriction for future runs.",
        ])
    elif category == "UNSUPPORTED_OPCODE":
        lines.extend([
            "1. Extract exact guest PC and opcode bytes from stderr/trace.",
            "2. Disassemble around the PC: `objdump -d --start-address=... --stop-address=...`.",
            "3. Identify the full opcode family (prefix + opcode2 + modrm if applicable).",
            "4. Implement in ALL four layers:",
            "   - `hb_decoder.h`: new `HB_INS_*` enum value",
            "   - `hb_ir.h`: new `HB_IR_*` enum value",
            "   - `hb_decode_x64.c`: opcode recognition with `parse_modrm`",
            "   - `hb_lift_x64.c`: mapping to IR",
            "   - `hb_interpreter.c`: correct semantics (lane-wise or memcpy for unaligned)",
            "5. Add regression tests in `hb_test_runner.c` covering:",
            "   - The exact trigger bytes",
            "   - Memory-displacement siblings",
            "   - Register-register siblings",
            "6. Run `./scripts/test-hyperbridge.sh` → all pass.",
            "7. Rebuild libhyperbridge.a, relink ntdll.so, codesign.",
            "8. Rerun smoke.",
        ])
    elif category == "MEMORY_FAULT":
        lines.extend([
            "1. Identify fault address and PC from stderr/trace.",
            "2. Check if fault address is near an IAT or native pointer.",
            "3. If IAT leak: fix thunk/native pointer marshalling.",
            "4. If true memory access: add bounds check or fix interpreter memory model.",
            "5. Add regression test reproducing the fault.",
            "6. Rebuild, relink, rerun.",
        ])
    elif category == "ABI_OR_RETURN_PATH":
        lines.extend([
            "1. Check `x64-signal-callback` status code.",
            "2. If `c000007b`: inspect PE imports for wrong-arch CRT dependency.",
            "3. If `c0000005`: check native pointer / stack alignment in callback trampoline.",
            "4. Do NOT add unrelated bounds checks.",
            "5. Add regression test for the callback path.",
        ])
    elif category == "HELPER_CAPTURE_FALSE_NEGATIVE":
        lines.extend([
            "1. Do NOT touch comctl32/ImageList or Wine DLLs.",
            "2. Fix the helper capture/readback classification logic.",
            "3. Ensure helper waits for readiness (visible window, non-empty title) before capture.",
            "4. Add CG-based validation where possible.",
            "5. Recompile helper and rerun.",
            "6. Require CG proof before declaring any visual metric green.",
        ])
    elif category == "UI_METRICS":
        lines.extend([
            "1. Inspect SM_CYMENU, NONCLIENTMETRICS, statusbar/editor rects.",
            "2. Fix generic Wine metrics/layout, not app-specific hardcodes.",
            "3. Do NOT mark green without Windows baseline artifact.",
            "4. Compare CG capture against known Windows screenshot baseline.",
            "5. Narrow to ExtTextOutW, FreeType integration, or glyph cache.",
        ])
    elif category == "VISUAL_BLACK_ARTIFACT":
        lines.extend([
            "1. Compare CG capture zoomed on the affected region.",
            "2. Trace WM_ERASEBKGND / FillRect / DrawThemeBackground for the affected part.",
            "3. Check winemac.drv dirty rect completeness (resize/minimize/restore test).",
            "4. Do NOT shrink window size blindly.",
            "5. Do NOT patch uxtheme/comctl32 without CG proof.",
        ])
    elif category == "GETICONINFO_COLOR_LOSS":
        lines.extend([
            "1. Trace HICON -> GetIconInfo -> DrawIconEx -> ImageList -> ListView/TreeView draw.",
            "2. Check mask inversion and alpha premultiplication.",
            "3. Do NOT patch shell32 icon loading.",
            "4. Fix the draw-time path in GDI or ImageList.",
        ])
    elif category == "PRODUCT_GREEN":
        lines.extend([
            "1. No runtime patches needed.",
            "2. Run clean exit probe.",
            "3. Collect performance baseline.",
            "4. Do NOT touch runtime unless new evidence appears.",
        ])
    else:
        lines.extend([
            "1. Collect more evidence: screenshots, traces, stderr, disassembly.",
            "2. Run targeted probes to narrow the blocker.",
            "3. Do NOT patch anything without a clear root cause.",
        ])

    lines.append("")

    # Evidence
    lines.append("## Evidence Paths")
    lines.append("")
    evidence = result.get("evidence_paths", {})
    for k, v in evidence.items():
        if v:
            lines.append(f"- `{k}`: {v}")
    if not any(evidence.values()):
        lines.append("- (no evidence paths recorded)")
    lines.append("")

    # Compat / visual sub-results
    lines.append("## Compatibility Findings")
    lines.append("")
    for f in compat.get("findings", []):
        lines.append(f"- `{f.get('category')}` ({f.get('layer')}): {f.get('detail')}")
    if not compat.get("findings"):
        lines.append("- (none)")
    lines.append("")

    lines.append("## Visual Findings")
    lines.append("")
    for f in visual.get("findings", []):
        lines.append(f"- `{f.get('class')}` ({f.get('layer')}): {f.get('detail')}")
    if not visual.get("findings"):
        lines.append("- (none)")
    lines.append("")

    # Acceptance criteria
    lines.append("## Acceptance Criteria")
    lines.append("")
    for crit in criteria:
        lines.append(f"- [ ] {crit}")
    lines.append("")

    # Rollback
    lines.append("## Rollback / Cleanup Rules")
    lines.append("")
    for rule in rollback:
        lines.append(f"- {rule}")
    if not rollback:
        lines.append("- (none specified)")
    lines.append("")

    # End
    lines.append("---")
    lines.append("")
    lines.append("Generated by `tools/ai_configurator/generate_codex_task.py`")
    lines.append("")

    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description="Codex Next Task Generator")
    parser.add_argument("--result", default="/Volumes/MacOS/MacRunner/reports/ai-configurator/latest-configurator-result.json",
                        help="Path to configurator result JSON")
    parser.add_argument("--out", default="/Volumes/MacOS/MacRunner/reports/ai-configurator/CODEX-NEXT-TASK.md",
                        help="Output markdown path")
    args = parser.parse_args()

    result = load_json(Path(args.result))
    task_md = generate_codex_task(result)

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(task_md)

    print(f"Codex next task written to: {out_path}")


if __name__ == "__main__":
    main()
