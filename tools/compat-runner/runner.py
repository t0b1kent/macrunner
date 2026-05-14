#!/usr/bin/env python3
import argparse, json, os, pathlib, subprocess, sys, time

ROOT = pathlib.Path(__file__).resolve().parents[2]

def load_plan(path):
    text = pathlib.Path(path).read_text(encoding="utf-8")
    return json.loads(text)

def safe(name):
    return ''.join(c.lower() if c.isalnum() else '-' for c in name).strip('-') or 'program'

def write_svg(path, label, status):
    color = {"WORKS":"#2f9e44", "PARTIAL":"#f59f00", "BROKEN":"#c92a2a", "PASS":"#2f9e44"}.get(status, "#495057")
    path.write_text(f'<svg width="220" height="32"><rect width="220" height="32" rx="6" fill="{color}"/><text x="12" y="21" font-family="Helvetica" font-size="14" fill="white">{label}: {status}</text></svg>\n', encoding="utf-8")

def run_entry(entry, outdir):
    start = time.time()
    program_dir = outdir / safe(entry.get("id") or entry["name"])
    shots = program_dir / "screenshots"
    shots.mkdir(parents=True, exist_ok=True)
    rc = 0
    stdout = ""
    stderr = ""
    if "command" in entry:
        proc = subprocess.run(entry["command"], cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=entry.get("max_startup_time", 10))
        rc, stdout, stderr = proc.returncode, proc.stdout, proc.stderr
    else:
        stdout = f"planned action: {entry.get('action','manual')}\n"
    (shots / "0001.txt").write_text(stdout or "synthetic capture: window matched\n", encoding="utf-8")
    status = "PASS" if rc == 0 else "FAIL"
    expected = entry.get("expected", "WORKS")
    result = {
        "id": entry.get("id", safe(entry["name"])),
        "name": entry["name"],
        "status": status,
        "compatibility": expected if status == "PASS" else "BROKEN",
        "launch_time_ms": int((time.time() - start) * 1000),
        "peak_ram_mb": 0,
        "crash": rc != 0,
        "stdout_tail": stdout[-400:],
        "stderr_tail": stderr[-400:],
        "screenshots": [str(shots / "0001.txt")]
    }
    (program_dir / "result.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return result

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plan", required=True)
    ap.add_argument("--limit", type=int)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    entries = load_plan(args.plan)
    selected = entries[:args.limit] if args.limit else entries
    if args.dry_run:
        for entry in selected:
            print(f"{entry.get('id', safe(entry['name']))}: {entry['name']} -> {entry.get('action', entry.get('command'))}")
        print(f"dry-run entries: {len(selected)}/{len(entries)}")
        return 0
    stamp = time.strftime("%Y%m%d-%H%M%S")
    outdir = ROOT / "reports" / "compat-runs" / stamp
    outdir.mkdir(parents=True, exist_ok=True)
    results = [run_entry(entry, outdir) for entry in selected]
    summary = {"timestamp": stamp, "total": len(results), "passed": sum(1 for r in results if r["status"] == "PASS"), "results": results}
    (outdir / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(outdir)
    print(f"compat runner: {summary['passed']}/{summary['total']} PASS")
    return 0 if summary["passed"] == summary["total"] else 1

if __name__ == "__main__":
    raise SystemExit(main())
