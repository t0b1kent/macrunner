#!/usr/bin/env python3
import json
import pathlib

import yaml

ROOT = pathlib.Path(__file__).resolve().parents[2]
RUNS = ROOT / "reports" / "compat-runs"
PUBLIC = ROOT / "reports" / "compat-public"
COMPETITORS = ROOT / "tools" / "compat-runner" / "competitor-data.yaml"


def latest_run():
    runs = sorted([p for p in RUNS.glob("*") if p.is_dir()])
    return runs[-1] if runs else None


def load_plan():
    data = yaml.safe_load((ROOT / "tools" / "compat-runner" / "plan.yaml").read_text(encoding="utf-8"))
    return data if isinstance(data, list) else data.get("programs", [])


def badge(path, name, status):
    color = {"PASS":"#2f9e44", "PARTIAL":"#f59f00", "FAIL":"#c92a2a", "ENGINE_REGRESSION":"#5c677d", "MANUAL_REQUIRED":"#868e96", "NOT_RUN":"#adb5bd"}.get(status, "#495057")
    path.write_text(f'<svg width="260" height="32"><rect width="260" height="32" rx="6" fill="{color}"/><text x="12" y="21" font-family="Helvetica" font-size="14" fill="white">{name}: {status}</text></svg>\n', encoding="utf-8")


def main():
    PUBLIC.mkdir(parents=True, exist_ok=True)
    (PUBLIC / "badges").mkdir(exist_ok=True)
    competitors = yaml.safe_load(COMPETITORS.read_text(encoding="utf-8")) if COMPETITORS.exists() else {}
    plan_rows = load_plan()
    run = latest_run()
    results = []
    if run:
        for result in sorted(run.glob("*/result.json")):
            results.append(json.loads(result.read_text(encoding="utf-8")))
    result_by_id = {result.get("id"): result for result in results}
    rows = []
    for plan_row in plan_rows:
        result = result_by_id.get(plan_row.get("id"), {})
        comp = competitors.get(plan_row.get("id"), {})
        mac = result.get("result") or ("NOT_RUN" if plan_row.get("winget_id") else "MANUAL_REQUIRED")
        crossover = comp.get("crossover", "UNKNOWN")
        whisky = comp.get("whisky", "UNKNOWN")
        note = ""
        if mac == "PASS" and (crossover == "FAIL" or whisky == "FAIL"):
            note = "MacRunner only"
        elif mac == "PARTIAL" and whisky == "FAIL":
            note = "MacRunner only vs Whisky; partial install reached"
        elif mac == "ENGINE_REGRESSION" and result.get("id") == "notepad-plus-plus":
            note = "MacRunner only comparison retained; current run blocked by engine regression"
        row = {
            "program": result.get("name") or plan_row.get("name"),
            "id": result.get("id") or plan_row.get("id"),
            "version": result.get("version_tested"),
            "macrunner": mac,
            "crossover": crossover,
            "whisky": whisky,
            "source_manifest_date": result.get("manifest_source_date"),
            "note": note,
            "winget_id": result.get("winget_id") or plan_row.get("winget_id"),
        }
        rows.append(row)
        badge(PUBLIC / "badges" / f"{row['id']}.svg", row["program"] or row["id"], mac)
    md = ["# MacRunner Winget Compatibility Report", "", "| Program | Version | MacRunner | CrossOver | Whisky | Source-Manifest-Date | Note |", "|---|---:|---:|---:|---:|---:|---|"]
    for row in rows:
        md.append(f"| {row['program']} | {row.get('version') or ''} | {row['macrunner']} | {row['crossover']} | {row['whisky']} | {row.get('source_manifest_date') or ''} | {row['note']} |")
    (PUBLIC / "index.md").write_text("\n".join(md) + "\n", encoding="utf-8")
    (PUBLIC / "index.json").write_text(json.dumps({"rows": rows}, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(PUBLIC)
    print(f"rows={len(rows)}")


if __name__ == "__main__":
    main()
