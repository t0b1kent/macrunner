#!/usr/bin/env python3
"""
MacRunner Visual Regression Lab — Screenshot / CG Capture Analyzer

Input:
  - PNG/BMP path
  - optional JSON window metadata
  - optional app profile

Output:
  - reports/visual-regression/latest-visual-analysis.json
  - reports/visual-regression/LATEST-VISUAL-ANALYSIS.md

Works on saved captures only. Does not require live Wine.
"""

import argparse
import json
import os
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Optional


THRESHOLDS = {
    "bottom_black_band_max_px": 8,
    "right_black_band_max_px": 8,
    "toolbar_colorful_floor": 900,
    "black_ratio_max": 0.15,
    "statusbar_black_ratio_max": 0.30,
    "scrollbar_black_ratio_max": 0.40,
    "band_contiguous_min_px": 4,
    "toolbar_region_top_pct": 0.20,
    "statusbar_region_bottom_px": 30,
    "scrollbar_region_right_px": 24,
    "editor_region_inset_px": 4,
}


def is_black(r: int, g: int, b: int, threshold: int = 20) -> bool:
    return max(r, g, b) <= threshold


def is_white(r: int, g: int, b: int, threshold: int = 235) -> bool:
    return min(r, g, b) >= threshold


def is_gray(r: int, g: int, b: int, color_threshold: int = 20) -> bool:
    return max(r, g, b) - min(r, g, b) <= color_threshold


def is_colorful(r: int, g: int, b: int, alpha: int, color_threshold: int = 20, alpha_threshold: int = 128) -> bool:
    return max(r, g, b) - min(r, g, b) > color_threshold and alpha >= alpha_threshold


def detect_bottom_band(pixels: list, width: int, height: int) -> dict:
    """Detect contiguous black band at bottom."""
    band_height = 0
    max_band = 0
    for y in range(height - 1, -1, -1):
        row_black = sum(1 for x in range(width) if is_black(*pixels[y * width + x][:3]))
        if row_black >= width * 0.85:
            band_height += 1
            max_band = max(max_band, band_height)
        else:
            break
    return {
        "band_height": max_band,
        "band_detected": max_band > THRESHOLDS["bottom_black_band_max_px"],
    }


def detect_right_band(pixels: list, width: int, height: int) -> dict:
    """Detect contiguous black band at right edge."""
    band_width = 0
    max_band = 0
    for x in range(width - 1, -1, -1):
        col_black = sum(1 for y in range(height) if is_black(*pixels[y * width + x][:3]))
        if col_black >= height * 0.85:
            band_width += 1
            max_band = max(max_band, band_width)
        else:
            break
    return {
        "band_width": max_band,
        "band_detected": max_band > THRESHOLDS["right_black_band_max_px"],
    }


def analyze_toolbar_region(pixels: list, width: int, height: int) -> dict:
    """Analyze top N% of image for toolbar color."""
    toolbar_h = int(height * THRESHOLDS["toolbar_region_top_pct"])
    total = width * toolbar_h
    if total == 0:
        return {"colorful_pixels": 0, "total_pixels": 0, "colorful_ratio": 0.0}
    colorful = sum(
        1 for y in range(toolbar_h) for x in range(width)
        if is_colorful(*pixels[y * width + x])
    )
    return {
        "colorful_pixels": colorful,
        "total_pixels": total,
        "colorful_ratio": colorful / total,
        "colorful_floor": THRESHOLDS["toolbar_colorful_floor"],
        "pass": colorful >= THRESHOLDS["toolbar_colorful_floor"],
    }


def analyze_statusbar_region(pixels: list, width: int, height: int) -> dict:
    """Analyze bottom ~30px for statusbar background."""
    sb_h = min(THRESHOLDS["statusbar_region_bottom_px"], height)
    total = width * sb_h
    if total == 0:
        return {"black_pixels": 0, "black_ratio": 0.0}
    black = sum(
        1 for y in range(height - sb_h, height) for x in range(width)
        if is_black(*pixels[y * width + x][:3])
    )
    ratio = black / total
    return {
        "black_pixels": black,
        "total_pixels": total,
        "black_ratio": ratio,
        "bad_background": ratio > THRESHOLDS["statusbar_black_ratio_max"],
    }


def analyze_scrollbar_region(pixels: list, width: int, height: int) -> dict:
    """Analyze right ~24px for scrollbar artifacts."""
    sb_w = min(THRESHOLDS["scrollbar_region_right_px"], width)
    total = sb_w * height
    if total == 0:
        return {"black_pixels": 0, "black_ratio": 0.0}
    black = sum(
        1 for y in range(height) for x in range(width - sb_w, width)
        if is_black(*pixels[y * width + x][:3])
    )
    ratio = black / total
    return {
        "black_pixels": black,
        "total_pixels": total,
        "black_ratio": ratio,
        "black_artifact": ratio > THRESHOLDS["scrollbar_black_ratio_max"],
    }


def analyze_image(image_path: Path) -> dict:
    ext = image_path.suffix.lower()
    try:
        from PIL import Image
        img = Image.open(image_path)
        # Always convert to RGBA so pixels are 4-tuples (R,G,B,A)
        if img.mode != "RGBA":
            img = img.convert("RGBA")
        pixels = list(img.getdata())
        width, height = img.size
    except Exception as e:
        return {"error": f"PIL failed to read image: {e}"}

    total_pixels = width * height
    black_pixels = sum(1 for p in pixels if is_black(*p[:3]))
    white_pixels = sum(1 for p in pixels if is_white(*p[:3]))
    gray_pixels = sum(1 for p in pixels if is_gray(*p[:3]) and not is_black(*p[:3]) and not is_white(*p[:3]))
    colorful_pixels = sum(1 for p in pixels if is_colorful(*p))

    result = {
        "image_path": str(image_path),
        "width": width,
        "height": height,
        "total_pixels": total_pixels,
        "black_pixels": black_pixels,
        "white_pixels": white_pixels,
        "gray_pixels": gray_pixels,
        "colorful_pixels": colorful_pixels,
        "black_ratio": black_pixels / total_pixels if total_pixels else 0,
        "colorful_ratio": colorful_pixels / total_pixels if total_pixels else 0,
        "thresholds": THRESHOLDS,
        "bottom_band": detect_bottom_band(pixels, width, height),
        "right_band": detect_right_band(pixels, width, height),
        "toolbar": analyze_toolbar_region(pixels, width, height),
        "statusbar": analyze_statusbar_region(pixels, width, height),
        "scrollbar": analyze_scrollbar_region(pixels, width, height),
    }

    return result


def write_outputs(result: dict, out_dir: Path):
    out_dir.mkdir(parents=True, exist_ok=True)
    json_path = out_dir / "latest-visual-analysis.json"
    md_path = out_dir / "LATEST-VISUAL-ANALYSIS.md"

    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2)

    lines = [
        "# Latest Visual Analysis",
        "",
        f"Image: `{result.get('image_path', 'N/A')}`",
        f"Dimensions: {result.get('width')} x {result.get('height')}",
        "",
        "## Global Metrics",
        "",
        f"- Total pixels: {result.get('total_pixels')}",
        f"- Black pixels: {result.get('black_pixels')} ({result.get('black_ratio', 0):.4f})",
        f"- Colorful pixels: {result.get('colorful_pixels')} ({result.get('colorful_ratio', 0):.4f})",
        "",
        "## Region Analysis",
        "",
        f"### Bottom Band",
        f"- Band height: {result['bottom_band']['band_height']} px",
        f"- Detected: {'YES' if result['bottom_band']['band_detected'] else 'NO'}",
        "",
        f"### Right Band",
        f"- Band width: {result['right_band']['band_width']} px",
        f"- Detected: {'YES' if result['right_band']['band_detected'] else 'NO'}",
        "",
        f"### Toolbar (top {int(THRESHOLDS['toolbar_region_top_pct'] * 100)}%)",
        f"- Colorful pixels: {result['toolbar']['colorful_pixels']} / {result['toolbar']['total_pixels']}",
        f"- Colorful ratio: {result['toolbar']['colorful_ratio']:.4f}",
        f"- Floor: {result['toolbar']['colorful_floor']}",
        f"- Pass: {'YES' if result['toolbar']['pass'] else 'NO'}",
        "",
        f"### Statusbar (bottom {THRESHOLDS['statusbar_region_bottom_px']} px)",
        f"- Black pixels: {result['statusbar']['black_pixels']} / {result['statusbar']['total_pixels']}",
        f"- Black ratio: {result['statusbar']['black_ratio']:.4f}",
        f"- Bad background: {'YES' if result['statusbar']['bad_background'] else 'NO'}",
        "",
        f"### Scrollbar (right {THRESHOLDS['scrollbar_region_right_px']} px)",
        f"- Black pixels: {result['scrollbar']['black_pixels']} / {result['scrollbar']['total_pixels']}",
        f"- Black ratio: {result['scrollbar']['black_ratio']:.4f}",
        f"- Black artifact: {'YES' if result['scrollbar']['black_artifact'] else 'NO'}",
        "",
    ]
    if "error" in result:
        lines.append(f"**Error:** {result['error']}")

    with open(md_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def main():
    parser = argparse.ArgumentParser(description="MacRunner Visual Capture Analyzer")
    parser.add_argument("image", help="Path to PNG/BMP capture")
    parser.add_argument("--metadata", help="Optional JSON window metadata")
    parser.add_argument("--profile", help="Optional app visual profile JSON")
    parser.add_argument("--out-dir", default="/Volumes/MacOS/MacRunner/reports/visual-regression",
                        help="Output directory")
    args = parser.parse_args()

    image_path = Path(args.image)
    if not image_path.exists():
        print(f"ERROR: image does not exist: {image_path}", file=sys.stderr)
        sys.exit(1)

    result = analyze_image(image_path)

    # Overlay metadata/profile thresholds if provided
    if args.metadata:
        try:
            with open(args.metadata, "r") as f:
                meta = json.load(f)
            result["metadata"] = meta
        except Exception as e:
            result["metadata_error"] = str(e)

    if args.profile:
        try:
            with open(args.profile, "r") as f:
                profile = json.load(f)
            result["profile"] = profile
            # Override thresholds with profile values
            for key, val in profile.get("thresholds", {}).items():
                if key in THRESHOLDS:
                    THRESHOLDS[key] = val
                    result["thresholds"][key] = val
            # Re-run analysis with updated thresholds
            result = analyze_image(image_path)
            if args.metadata:
                result["metadata"] = meta
            result["profile"] = profile
        except Exception as e:
            result["profile_error"] = str(e)

    result["analyzed_at"] = datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")
    write_outputs(result, Path(args.out_dir))

    print(f"Analysis complete.")
    print(f"  JSON: {args.out_dir}/latest-visual-analysis.json")
    print(f"  MD:   {args.out_dir}/LATEST-VISUAL-ANALYSIS.md")
    if "error" in result:
        print(f"  Error: {result['error']}")
        sys.exit(1)


if __name__ == "__main__":
    main()
