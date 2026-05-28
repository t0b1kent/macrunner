#!/usr/bin/env python3
"""
MacRunner Visual Regression Lab — Visual State Classifier

Input:
  - latest-artifacts.json
  - visual analysis JSON (from analyze_capture.py)
  - profiles/visual/<app>-visual-profile.json

Output:
  - reports/visual-regression/latest-visual-classification.json
  - reports/visual-regression/LATEST-VISUAL-CLASSIFICATION.md
"""

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


VISUAL_CLASSES = [
    "VISUAL_PASS",
    "VISUAL_FAIL_BOTTOM_BAND",
    "VISUAL_FAIL_RIGHT_BAND",
    "VISUAL_FAIL_SCROLLBAR_BLACK",
    "VISUAL_FAIL_FOLDER_ICONS_BLACK",
    "VISUAL_FAIL_WINDOW_BUTTONS_EMPTY",
    "VISUAL_FAIL_TAB_BLACK_SQUARES",
    "VISUAL_FAIL_STATUSBAR",
    "VISUAL_FAIL_UI_METRICS",
    "VISUAL_INCONCLUSIVE",
    "CAPTURE_MISSING",
]


def load_json(path: Path) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def classify(analysis: dict, profile: dict) -> dict:
    result = {
        "classified_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "image_path": analysis.get("image_path"),
        "findings": [],
        "primary_class": "VISUAL_INCONCLUSIVE",
        "confidence": "LOW",
        "codex_next_action": None,
        "likely_root_layer": None,
    }

    if "error" in analysis:
        result["primary_class"] = "CAPTURE_MISSING"
        result["findings"].append({
            "class": "CAPTURE_MISSING",
            "detail": f"Capture analysis failed: {analysis['error']}",
            "layer": "HELPER",
        })
        result["confidence"] = "HIGH"
        result["likely_root_layer"] = "HELPER"
        result["codex_next_action"] = "Check capture permissions, retry with readiness delay, verify window visibility."
        return result

    thresholds = profile.get("thresholds", {})
    findings = []

    # Bottom band
    bb = analysis.get("bottom_band", {})
    if bb.get("band_detected"):
        findings.append({
            "class": "VISUAL_FAIL_BOTTOM_BAND",
            "detail": f"bottom_band_height={bb.get('band_height')}px exceeds max={thresholds.get('bottom_black_band_max_px', 8)}",
            "layer": "WINE",
            "root_candidates": ["statusbar paint", "background erase", "dirty rect", "layout"],
        })

    # Right band
    rb = analysis.get("right_band", {})
    if rb.get("band_detected"):
        findings.append({
            "class": "VISUAL_FAIL_RIGHT_BAND",
            "detail": f"right_band_width={rb.get('band_width')}px exceeds max={thresholds.get('right_black_band_max_px', 8)}",
            "layer": "WINE",
            "root_candidates": ["scrollbar background", "editor not filling client width", "dirty rect"],
        })

    # Scrollbar
    sb = analysis.get("scrollbar", {})
    if sb.get("black_artifact"):
        findings.append({
            "class": "VISUAL_FAIL_SCROLLBAR_BLACK",
            "detail": f"scrollbar_black_ratio={sb.get('black_ratio', 0):.4f} exceeds max={thresholds.get('scrollbar_black_ratio_max', 0.40)}",
            "layer": "WINE_UXTHEME",
            "root_candidates": ["scrollbar part/state drawing", "pPatBlt ROP=0", "mask/alpha"],
        })

    # Empty MDI/window control glyph boxes
    wb = analysis.get("window_buttons", {})
    if wb.get("empty_buttons_detected"):
        findings.append({
            "class": "VISUAL_FAIL_WINDOW_BUTTONS_EMPTY",
            "detail": f"empty_window_button_squares={wb.get('empty_square_count')} components={wb.get('components', [])[:3]}",
            "layer": "WINE_WIN32U_MARLETT",
            "root_candidates": ["Marlett font load", "caption glyph fallback", "MDI menubar magic bitmap drawing"],
        })

    # Tab close/modified black squares
    tabs = analysis.get("tabs", {})
    if tabs.get("black_squares_detected"):
        findings.append({
            "class": "VISUAL_FAIL_TAB_BLACK_SQUARES",
            "detail": f"tab_black_square_components={tabs.get('black_square_count')} components={tabs.get('components', [])[:3]}",
            "layer": "WINE_COMCTL32",
            "root_candidates": ["ImageList ILD_TRANSPARENT mask draw", "CopyImage monochrome placeholder", "tab owner-draw imagelist"],
        })

    # Shell folder/drive icons in file dialogs
    folder = analysis.get("folder_icons", {})
    if folder.get("checked") and not folder.get("pass", True):
        findings.append({
            "class": "VISUAL_FAIL_FOLDER_ICONS_BLACK",
            "detail": (
                f"folder_icon_colorful_pixels={folder.get('colorful_pixels')} "
                f"floor={folder.get('colorful_floor')} black_square_components={folder.get('black_square_count')}"
            ),
            "layer": "WINE_USER32_CURSORICON",
            "root_candidates": ["indexed ICO StretchDIBits conversion", "shell32 system image list", "HICON mask/color extraction"],
        })

    # Statusbar
    st = analysis.get("statusbar", {})
    if st.get("bad_background"):
        findings.append({
            "class": "VISUAL_FAIL_STATUSBAR",
            "detail": f"statusbar_black_ratio={st.get('black_ratio', 0):.4f} exceeds max={thresholds.get('statusbar_black_ratio_max', 0.30)}",
            "layer": "WINE_UXTHEME",
            "root_candidates": ["statusbar background erase", "DrawThemeBackground part/state"],
        })

    # Toolbar low color
    tb = analysis.get("toolbar", {})
    if not tb.get("pass", True):
        # Distinguish real defect from helper false-negative
        cg_pass_helper_fail = False
        # If we don't have helper data, treat as inconclusive
        if tb.get("colorful_pixels", 0) > 0 and tb.get("colorful_pixels", 0) < tb.get("colorful_floor", 900):
            findings.append({
                "class": "VISUAL_FAIL_UI_METRICS",
                "detail": f"toolbar_colorful_pixels={tb.get('colorful_pixels')} below floor={tb.get('colorful_floor')}",
                "layer": "WINE_COMCTL32",
                "root_candidates": ["ImageList fill", "toolbar background", "GetIconInfo color loss"],
            })

    # Global black ratio
    black_ratio = analysis.get("black_ratio", 0)
    black_max = thresholds.get("black_ratio_max", 0.15)
    if black_ratio > black_max and not findings:
        findings.append({
            "class": "VISUAL_FAIL_UI_METRICS",
            "detail": f"global_black_ratio={black_ratio:.4f} exceeds max={black_max}",
            "layer": "WINE_GDI",
            "root_candidates": ["background erase", "theme background", "DIB color table"],
        })

    result["findings"] = findings

    if not findings:
        # No visual defects detected
        result["primary_class"] = "VISUAL_PASS"
        result["confidence"] = "MEDIUM"
        result["codex_next_action"] = "No visual defects detected by automated analysis. Verify with manual review before marking green. Move to clean exit / performance tests."
        result["likely_root_layer"] = None
    else:
        # Priority: WINE layout > WINE GDI > WINE comctl32 > WINE uxtheme > HELPER > UNKNOWN
        priority = {
            "VISUAL_FAIL_BOTTOM_BAND": 1,
            "VISUAL_FAIL_RIGHT_BAND": 2,
            "VISUAL_FAIL_UI_METRICS": 3,
            "VISUAL_FAIL_WINDOW_BUTTONS_EMPTY": 4,
            "VISUAL_FAIL_TAB_BLACK_SQUARES": 5,
            "VISUAL_FAIL_SCROLLBAR_BLACK": 6,
            "VISUAL_FAIL_STATUSBAR": 7,
            "VISUAL_FAIL_FOLDER_ICONS_BLACK": 8,
            "VISUAL_INCONCLUSIVE": 99,
            "CAPTURE_MISSING": 99,
        }
        sorted_findings = sorted(findings, key=lambda x: priority.get(x["class"], 99))
        primary = sorted_findings[0]
        result["primary_class"] = primary["class"]
        result["likely_root_layer"] = primary["layer"]
        result["confidence"] = "HIGH" if len(findings) >= 2 else "MEDIUM"

        # Generate Codex next action
        result["codex_next_action"] = generate_next_action(primary)

    return result


def generate_next_action(primary: dict) -> str:
    cls = primary["class"]
    layer = primary["layer"]
    detail = primary["detail"]

    actions = {
        "VISUAL_FAIL_BOTTOM_BAND": (
            "1. Compare client rect / editor rect / statusbar rect against CG band.\n"
            "2. Trace WM_ERASEBKGND, FillRect, DrawThemeBackground for statusbar part.\n"
            "3. Check if winemac.drv dirty rect is incomplete.\n"
            "4. Do NOT shrink window size blindly."
        ),
        "VISUAL_FAIL_RIGHT_BAND": (
            "1. Compare editor width against client width minus scrollbar width.\n"
            "2. Trace scrollbar DrawThemeBackground calls.\n"
            "3. Check if editor HWND is intentionally smaller (docking panels)."
        ),
        "VISUAL_FAIL_SCROLLBAR_BLACK": (
            "1. Zoom CG capture on scrollbar region.\n"
            "2. Trace uxtheme scrollbar part DrawThemeBackground destination HDC.\n"
            "3. Check pPatBlt with ROP=0 in gdi32/bitblt.c.\n"
            "4. Do NOT patch uxtheme blindly."
        ),
        "VISUAL_FAIL_WINDOW_BUTTONS_EMPTY": (
            "1. Trace Marlett font load and glyph index for 0x30/0x31/0x32/0x72.\n"
            "2. Verify MDI menubar magic bitmap drawing overlays caption glyph fallback.\n"
            "3. Re-run CG capture and require empty_square_count=0."
        ),
        "VISUAL_FAIL_TAB_BLACK_SQUARES": (
            "1. Trace ImageList_DrawIndirect ILD_TRANSPARENT mask path.\n"
            "2. Verify temp bitmap uses destination background and CopyImage black placeholder fallback is active.\n"
            "3. Re-run CG capture and require tab_black_square_components=0."
        ),
        "VISUAL_FAIL_STATUSBAR": (
            "1. Trace WM_ERASEBKGND -> DefWindowProc -> FillRect / DrawThemeBackground.\n"
            "2. Compare against Windows baseline if available.\n"
            "3. Check statusbar part/state ID correctness."
        ),
        "VISUAL_FAIL_UI_METRICS": (
            "1. If toolbar: compare CG vs helper capture. CG ground truth first.\n"
            "2. Trace ImageList_Draw, GetIconInfo, BitBlt mask/alpha path.\n"
            "3. Do NOT instrument comctl32/toolbar.c directly.\n"
            "4. If global: check background erase and DIB color table."
        ),
        "VISUAL_FAIL_FOLDER_ICONS_BLACK": (
            "1. Capture dialog CG when open.\n"
            "2. Trace HICON -> GetIconInfo -> DrawIconEx -> ImageList -> ListView draw.\n"
            "3. Check mask inversion and alpha premultiplication."
        ),
    }
    return actions.get(cls, "Collect more evidence (screenshots, traces, disassembly).")


def write_outputs(result: dict, out_dir: Path):
    out_dir.mkdir(parents=True, exist_ok=True)
    json_path = out_dir / "latest-visual-classification.json"
    md_path = out_dir / "LATEST-VISUAL-CLASSIFICATION.md"

    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2)

    lines = [
        "# Latest Visual Classification",
        "",
        f"Classified at: {result['classified_at']}",
        f"Image: `{result.get('image_path', 'N/A')}`",
        "",
        f"**Primary class:** `{result['primary_class']}`",
        f"**Confidence:** {result['confidence']}",
    ]
    if result.get("likely_root_layer"):
        lines.append(f"**Likely root layer:** `{result['likely_root_layer']}`")
    lines.append("")
    lines.append("## Findings")
    lines.append("")
    for f in result["findings"]:
        lines.append(f"- **{f['class']}** ({f['layer']}): {f['detail']}")
        if f.get("root_candidates"):
            lines.append(f"  - Root candidates: {', '.join(f['root_candidates'])}")
    lines.append("")
    lines.append("## Codex Next Action")
    lines.append("")
    lines.append(result.get("codex_next_action", "Collect more evidence."))
    lines.append("")

    with open(md_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def main():
    parser = argparse.ArgumentParser(description="MacRunner Visual State Classifier")
    parser.add_argument("--artifacts", default="/Volumes/MacOS/MacRunner/reports/visual-regression/latest-artifacts.json",
                        help="Path to latest-artifacts.json")
    parser.add_argument("--analysis", default="/Volumes/MacOS/MacRunner/reports/visual-regression/latest-visual-analysis.json",
                        help="Path to visual analysis JSON")
    parser.add_argument("--profile", default="/Volumes/MacOS/MacRunner/profiles/visual/notepadpp-visual-profile.json",
                        help="Path to app visual profile")
    parser.add_argument("--out-dir", default="/Volumes/MacOS/MacRunner/reports/visual-regression",
                        help="Output directory")
    args = parser.parse_args()

    analysis = load_json(Path(args.analysis)) if Path(args.analysis).exists() else {}
    profile = load_json(Path(args.profile)) if Path(args.profile).exists() else {}

    result = classify(analysis, profile)
    write_outputs(result, Path(args.out_dir))

    print(f"Classification complete.")
    print(f"  Primary: {result['primary_class']} ({result['confidence']})")
    print(f"  JSON: {args.out_dir}/latest-visual-classification.json")
    print(f"  MD:   {args.out_dir}/LATEST-VISUAL-CLASSIFICATION.md")


if __name__ == "__main__":
    main()
