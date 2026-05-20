#!/usr/bin/env python3
"""
MacRunner Visual Regression Lab — Codex Visual Fixpack Generator

Input:
  - latest-visual-classification.json
  - profiles/visual/<app>-visual-profile.json
  - visual-artifact-taxonomy.json

Output:
  - reports/visual-regression/CODEX-VISUAL-NEXT-FIXPACK.md
"""

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path


def load_json(path: Path) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def generate_fixpack(classification: dict, profile: dict, taxonomy: list) -> str:
    primary = classification.get("primary_class", "VISUAL_INCONCLUSIVE")
    findings = classification.get("findings", [])
    app_name = profile.get("app_name", "UnknownApp")

    lines = [
        "# Codex Visual Next Fixpack",
        "",
        f"**App:** {app_name}",
        f"**Primary class:** `{primary}`",
        f"**Generated at:** {datetime.now(timezone.utc).isoformat().replace('+00:00', 'Z')}",
        "",
        "## Classification Summary",
        "",
    ]
    for f in findings:
        lines.append(f"- **{f['class']}** ({f['layer']}): {f['detail']}")
    lines.append("")

    lines.append("## Fixpack Actions")
    lines.append("")

    if primary == "VISUAL_PASS":
        lines.append("### Visual: PASS")
        lines.append("1. No automated visual defects detected.")
        lines.append("2. Manual review still required before marking green.")
        lines.append("3. Move to clean exit / performance / stress tests.")
        lines.append("4. Do NOT touch visual runtime unless new evidence appears.")

    elif primary == "VISUAL_FAIL_BOTTOM_BAND":
        lines.append("### Visual: Bottom Black Band")
        lines.append("1. Compare client rect / editor rect / statusbar rect against CG band.")
        lines.append("2. Root candidates: layout, statusbar paint, dirty rect, background erase.")
        lines.append("3. Trace WM_ERASEBKGND → DefWindowProc → FillRect / DrawThemeBackground.")
        lines.append("4. Check if winemac.drv dirty rect is incomplete (resize/minimize/restore test).")
        lines.append("5. Do NOT shrink window size blindly.")

    elif primary == "VISUAL_FAIL_RIGHT_BAND":
        lines.append("### Visual: Right Black Band")
        lines.append("1. Compare editor width against client width minus scrollbar width.")
        lines.append("2. Trace scrollbar DrawThemeBackground calls.")
        lines.append("3. Check if editor HWND is intentionally smaller (docking panels, sidebars).")

    elif primary == "VISUAL_FAIL_SCROLLBAR_BLACK":
        lines.append("### Visual: Scrollbar Black Artifact")
        lines.append("1. Zoom CG capture on scrollbar region.")
        lines.append("2. Trace uxtheme scrollbar part DrawThemeBackground destination HDC.")
        lines.append("3. Check pPatBlt with ROP=0 in gdi32/bitblt.c.")
        lines.append("4. Do NOT patch uxtheme blindly; verify part/state ID and destination DIB.")

    elif primary == "VISUAL_FAIL_FOLDER_ICONS_BLACK":
        lines.append("### Visual: Folder Icons Black")
        lines.append("1. Capture dialog CG when open.")
        lines.append("2. Trace HICON → GetIconInfo → DrawIconEx → ImageList → ListView/TreeView draw.")
        lines.append("3. Check mask inversion and alpha premultiplication.")
        lines.append("4. Do NOT patch shell32 icon loading; fix the draw-time path.")

    elif primary == "VISUAL_FAIL_STATUSBAR":
        lines.append("### Visual: Statusbar Bad Background")
        lines.append("1. Trace WM_ERASEBKGND → DefWindowProc → FillRect / DrawThemeBackground for statusbar part.")
        lines.append("2. Compare against Windows baseline if available.")
        lines.append("3. Check statusbar part/state ID correctness.")

    elif primary == "VISUAL_FAIL_UI_METRICS":
        lines.append("### Visual: UI Metrics / Toolbar Low Color")
        lines.append("1. Compare CG visual vs helper capture. CG is ground truth.")
        lines.append("2. If CG < floor: root candidates = ImageList mask/alpha, GetIconInfo, comctl32 fill.")
        lines.append("3. If CG >= floor but helper shows black: fix helper readback, NOT runtime.")
        lines.append("4. Do NOT instrument comctl32/toolbar.c directly (c000007b regressions).")
        lines.append("5. Trace ImageList_Draw → _Draw → BitBlt SRCAND/SRCPAINT. Check alpha-as-mask bug.")

    elif primary == "CAPTURE_MISSING":
        lines.append("### Visual: Capture Missing")
        lines.append("1. Check sandbox permissions for screencapture.")
        lines.append("2. Verify window is visible and titled before capture.")
        lines.append("3. Add retry with longer readiness delay.")
        lines.append("4. Do NOT classify as product failure.")

    else:
        lines.append("### Visual: Inconclusive")
        lines.append("1. Collect more evidence: screenshots, zoomed captures, traces.")
        lines.append("2. Run all visual probes: main_window_cg_capture, toolbar_visual_cg, ui_metrics.")
        lines.append("3. Update visual profile thresholds if needed.")

    lines.append("")
    lines.append("## Forbidden Actions")
    lines.append("")
    for rule in profile.get("forbidden_actions", []):
        lines.append(f"- {rule}")
    lines.append("")

    lines.append("## Known Visual Defects for This App")
    lines.append("")
    for defect in profile.get("known_visual_defects", []):
        status = defect.get("status", "UNKNOWN")
        lines.append(f"- `{defect['class_id']}` {defect['name']}: **{status}** — {defect.get('note', '')}")
    lines.append("")

    lines.append("## Relevant Taxonomy Entries")
    lines.append("")
    for art in taxonomy:
        if any(f["class"].replace("VISUAL_FAIL_", "").replace("_", " ").lower() in art["name"].lower() for f in findings):
            lines.append(f"- `{art['class_id']}` {art['name']}: {art['description'][:100]}...")
    lines.append("")

    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description="Codex Visual Fixpack Generator")
    parser.add_argument("--classification", required=True, help="Path to latest-visual-classification.json")
    parser.add_argument("--profile", default="/Volumes/MacOS/MacRunner/profiles/visual/notepadpp-visual-profile.json",
                        help="Path to app visual profile")
    parser.add_argument("--taxonomy", default="/Volumes/MacOS/MacRunner/reports/visual-regression/visual-artifact-taxonomy.json",
                        help="Path to visual artifact taxonomy")
    parser.add_argument("--out", default="/Volumes/MacOS/MacRunner/reports/visual-regression/CODEX-VISUAL-NEXT-FIXPACK.md",
                        help="Output markdown path")
    args = parser.parse_args()

    classification = load_json(Path(args.classification))
    profile = load_json(Path(args.profile))

    taxonomy = []
    if Path(args.taxonomy).exists():
        db = load_json(Path(args.taxonomy))
        taxonomy = db.get("artifacts", [])

    fixpack = generate_fixpack(classification, profile, taxonomy)

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(fixpack)

    print(f"Visual fixpack written to: {out_path}")


if __name__ == "__main__":
    main()
