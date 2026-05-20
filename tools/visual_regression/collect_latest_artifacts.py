#!/usr/bin/env python3
"""
MacRunner Visual Regression Lab — Latest Artifact Collector

Finds the latest Notepad++ run artifacts without copying images.
Only references paths in JSON/Markdown.

Output:
  - reports/visual-regression/latest-artifacts.json
  - reports/visual-regression/LATEST-ARTIFACTS.md
"""

import json
import os
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional


REPORTS_DIR = Path("/Volumes/MacOS/MacRunner/reports")
PHASE_H_DIR = REPORTS_DIR / "phase-h"
COMPAT_DIR = REPORTS_DIR / "compat-learning"


def latest_dir(pattern: str, base: Path = PHASE_H_DIR) -> Optional[Path]:
    matches = sorted(base.glob(pattern), key=lambda p: p.stat().st_mtime, reverse=True)
    return matches[0] if matches else None


def find_file(run_dir: Path, pattern: str) -> Optional[Path]:
    if not run_dir.exists():
        return None
    matches = list(run_dir.glob(pattern))
    return matches[0] if matches else None


def collect() -> dict:
    result = {
        "collected_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "runs": {},
        "captures": {},
        "classifications": {},
    }

    # Latest npp-x64 run directories (limit 5)
    npp_dirs = sorted(PHASE_H_DIR.glob("npp-x64-*"), key=lambda p: p.stat().st_mtime, reverse=True)[:5]
    for d in npp_dirs:
        run_name = d.name
        run_data = {
            "path": str(d),
            "ui_smoke_result": str(find_file(d, "ui-smoke.result.txt")) if find_file(d, "ui-smoke.result.txt") else None,
            "ui_smoke_summary": str(find_file(d, "ui-smoke.summary.txt")) if find_file(d, "ui-smoke.summary.txt") else None,
            "stderr_log": str(find_file(d, "stderr.log")) if find_file(d, "stderr.log") else None,
            "exit_code": str(find_file(d, "exit.code")) if find_file(d, "exit.code") else None,
            "infra_status": str(find_file(d, "infra.status")) if find_file(d, "infra.status") else None,
        }

        # Look for captures
        toolbar_capture = find_file(d, "toolbar-capture.bmp")
        if toolbar_capture:
            run_data["toolbar_capture_bmp"] = str(toolbar_capture)
        toolbar_cg = find_file(d, "toolbar-cg-capture.bmp")
        if toolbar_cg:
            run_data["toolbar_cg_bmp"] = str(toolbar_cg)
        full_window = find_file(d, "full-window*.png") or find_file(d, "full-window*.bmp")
        if full_window:
            run_data["full_window_capture"] = str(full_window)
        window = find_file(d, "window.png")
        if window:
            run_data["window_png"] = str(window)
        cg_window = find_file(d, "cg/window.png")
        if cg_window:
            run_data["cg_window_png"] = str(cg_window)
        readiness = find_file(d, "readiness-timeline.json")
        if readiness:
            run_data["readiness_timeline_json"] = str(readiness)

        result["runs"][run_name] = run_data

    # Latest compat-learning classification
    latest_class = COMPAT_DIR / "latest-classification.json"
    if latest_class.exists():
        result["classifications"]["latest"] = str(latest_class)

    # Dashboard references
    dashboard_md = REPORTS_DIR / "visual-regression" / "VISUAL-REGRESSION-DASHBOARD.md"
    if dashboard_md.exists():
        result["dashboard_md"] = str(dashboard_md)

    taxonomy = REPORTS_DIR / "visual-regression" / "VISUAL-ARTIFACT-TAXONOMY.md"
    if taxonomy.exists():
        result["taxonomy_md"] = str(taxonomy)

    return result


def write_outputs(result: dict, out_dir: Path):
    out_dir.mkdir(parents=True, exist_ok=True)
    json_path = out_dir / "latest-artifacts.json"
    md_path = out_dir / "LATEST-ARTIFACTS.md"

    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2)

    lines = [
        "# Latest Artifacts",
        "",
        f"Collected at: {result['collected_at']}",
        "",
        "## Latest Runs",
        "",
    ]
    for run_name, run_data in result["runs"].items():
        lines.append(f"### {run_name}")
        lines.append(f"- Path: `{run_data['path']}`")
        if run_data.get("ui_smoke_result"):
            lines.append(f"- ui-smoke.result.txt: `{run_data['ui_smoke_result']}`")
        if run_data.get("toolbar_cg_bmp"):
            lines.append(f"- toolbar CG capture: `{run_data['toolbar_cg_bmp']}`")
        if run_data.get("full_window_capture"):
            lines.append(f"- full window capture: `{run_data['full_window_capture']}`")
        if run_data.get("window_png"):
            lines.append(f"- window.png: `{run_data['window_png']}`")
        if run_data.get("stderr_log"):
            lines.append(f"- stderr.log: `{run_data['stderr_log']}`")
        lines.append("")

    if result.get("classifications"):
        lines.append("## Classifications")
        for name, path in result["classifications"].items():
            lines.append(f"- {name}: `{path}`")
        lines.append("")

    if result.get("taxonomy_md"):
        lines.append(f"- Taxonomy: `{result['taxonomy_md']}`")
    if result.get("dashboard_md"):
        lines.append(f"- Dashboard: `{result['dashboard_md']}`")
    lines.append("")

    with open(md_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def main():
    result = collect()
    out_dir = Path("/Volumes/MacOS/MacRunner/reports/visual-regression")
    write_outputs(result, out_dir)
    print(f"Artifacts collected.")
    print(f"  JSON: {out_dir}/latest-artifacts.json")
    print(f"  MD:   {out_dir}/LATEST-ARTIFACTS.md")
    print(f"  Runs: {len(result['runs'])}")


if __name__ == "__main__":
    main()
