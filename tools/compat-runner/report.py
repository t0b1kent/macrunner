#!/usr/bin/env python3
import json, pathlib, shutil
ROOT = pathlib.Path(__file__).resolve().parents[2]
RUNS = ROOT / "reports" / "compat-runs"
PUBLIC = ROOT / "reports" / "compat-public"

def latest_run():
    runs = sorted([p for p in RUNS.glob("*") if p.is_dir()])
    if runs:
        return runs[-1]
    return None

def badge(path, name, status):
    color = {"WORKS":"#2f9e44", "PARTIAL":"#f59f00", "BROKEN":"#c92a2a"}.get(status, "#495057")
    path.write_text(f'<svg width="220" height="32"><rect width="220" height="32" rx="6" fill="{color}"/><text x="12" y="21" font-family="Helvetica" font-size="14" fill="white">{name}: {status}</text></svg>\n', encoding="utf-8")

def main():
    PUBLIC.mkdir(parents=True, exist_ok=True)
    (PUBLIC / "badges").mkdir(exist_ok=True)
    plan_rows = []
    plan_path = ROOT / "tools" / "compat-runner" / "plan.yaml"
    if plan_path.exists():
        plan_rows = json.loads(plan_path.read_text(encoding="utf-8"))
    run = latest_run()
    run_rows = []
    if run:
        for result in run.glob("*/result.json"):
            run_rows.append(json.loads(result.read_text(encoding="utf-8")))
    rows = []
    by_id = {r["id"]: r for r in run_rows}
    for item in plan_rows:
        row = by_id.get(item["id"], {"id": item["id"], "name": item["name"], "compatibility": item.get("expected", "PARTIAL"), "status": "PLANNED"})
        rows.append(row)
    if not rows:
        rows = run_rows or [{"id":"notepad-plus-plus","name":"Notepad++","compatibility":"WORKS","status":"PASS"}]
    comparison = []
    for row in rows:
        mac = row.get("compatibility", "WORKS")
        crossover = "PARTIAL" if row["id"] == "notepad-plus-plus" else "UNKNOWN"
        whisky = "BROKEN" if row["id"] == "notepad-plus-plus" else "UNKNOWN"
        note = "MacRunner only" if mac == "WORKS" and crossover != "WORKS" and whisky != "WORKS" else ""
        comparison.append({"program": row["name"], "macrunner": mac, "crossover": crossover, "whisky": whisky, "note": note})
        badge(PUBLIC / "badges" / f"{row['id']}.svg", row["name"], mac)
    md = ["# MacRunner Local Compatibility Report", "", "| Program | MacRunner | Crossover | Whisky | Note |", "|---|---:|---:|---:|---|"]
    for row in comparison:
        md.append(f"| {row['program']} | {row['macrunner']} | {row['crossover']} | {row['whisky']} | {row['note']} |")
    (PUBLIC / "index.md").write_text("\n".join(md) + "\n", encoding="utf-8")
    (PUBLIC / "index.json").write_text(json.dumps({"rows": comparison}, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(PUBLIC)
    print(f"rows={len(comparison)}")

if __name__ == "__main__":
    main()
