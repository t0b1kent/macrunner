#!/usr/bin/env python3
import argparse
import json
import os
import pathlib
import subprocess
import time

import yaml
from downloader import download_item
from manifest_reader import resolve

ROOT = pathlib.Path(__file__).resolve().parents[2]


def load_plan(path):
    data = yaml.safe_load(pathlib.Path(path).read_text(encoding="utf-8"))
    return data if isinstance(data, list) else data.get("programs", [])


def safe(name):
    return ''.join(c.lower() if c.isalnum() else '-' for c in name).strip('-') or 'program'


def engine_state():
    env = os.environ.copy()
    env["LOOP_COUNT"] = "1"
    try:
        proc = subprocess.run(["./scripts/loop-wineboot.sh"], cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=130)
        line = (proc.stdout.strip().splitlines() or [""])[-1]
        healthy = proc.returncode == 0 and "pass=1" in line and "fail=0" in line and "hang=0" in line
        return {"healthy": healthy, "label": line or f"rc={proc.returncode}", "stdout": proc.stdout[-600:], "stderr": proc.stderr[-600:]}
    except subprocess.TimeoutExpired:
        return {"healthy": False, "label": "loop-wineboot-timeout", "stdout": "", "stderr": "timeout"}


def run_entry(entry, outdir, engine):
    start = time.time()
    program_dir = outdir / safe(entry.get("id") or entry.get("winget_id") or entry["name"])
    program_dir.mkdir(parents=True, exist_ok=True)
    result = {
        "id": entry.get("id"),
        "name": entry.get("name", entry.get("id")),
        "winget_id": entry.get("winget_id"),
        "version_tested": None,
        "manifest_source_date": None,
        "downloaded": False,
        "downloaded_sha256_match": None,
        "install_started": False,
        "install_exit_code": None,
        "window_title_matched": False,
        "elapsed_s": 0,
        "result": "MANUAL_REQUIRED" if not entry.get("winget_id") else "PENDING",
        "engine_state": engine["label"],
        "screenshots": [],
    }
    if not entry.get("winget_id"):
        result["elapsed_s"] = round(time.time() - start, 3)
        (program_dir / "result.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return result
    try:
        manifest = resolve(entry["winget_id"])
        result["version_tested"] = manifest.get("latest_version") or manifest.get("version")
        result["manifest_source_date"] = manifest.get("manifest_source_date")
        if not engine["healthy"]:
            result["result"] = "ENGINE_REGRESSION"
            result["elapsed_s"] = round(time.time() - start, 3)
            (program_dir / "result.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
            return result
        installer, _state, _digest = download_item(manifest)
        result["downloaded"] = True
        result["downloaded_sha256_match"] = True
        bottle = pathlib.Path(os.environ.get("MACRUNNER_BOTTLES_ROOT", str(ROOT / "bottles"))) / f"compat-test-{safe(entry['id'])}"
        subprocess.run(["rm", "-rf", str(bottle)], check=False)
        bottle.mkdir(parents=True, exist_ok=True)
        env = os.environ.copy()
        env["WINEPREFIX"] = str(bottle)
        wine = env.get("MACRUNNER_WINE_BIN", str(ROOT / "engine/wine/dist/bin/wine"))
        subprocess.run([wine, "wineboot", "--init"], cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)
        args = [wine, str(installer)] + [a for a in (manifest.get("silent_args") or "").split() if a]
        proc = subprocess.run(args, cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=entry.get("timeout_s", 60))
        result["install_started"] = True
        result["install_exit_code"] = proc.returncode
        result["result"] = "PASS" if proc.returncode == 0 else "PARTIAL"
    except Exception as exc:
        result["result"] = "FAIL"
        result["error"] = str(exc)
    finally:
        result["elapsed_s"] = round(time.time() - start, 3)
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
            print(f"{entry.get('id')}: {entry.get('name')} -> {entry.get('winget_id', entry.get('media', 'manual-required'))}")
        print(f"dry-run entries: {len(selected)}/{len(entries)}")
        return 0
    stamp = time.strftime("%Y%m%d-%H%M%S")
    outdir = ROOT / "reports" / "compat-runs" / stamp
    outdir.mkdir(parents=True, exist_ok=True)
    engine = engine_state()
    results = [run_entry(entry, outdir, engine) for entry in selected]
    summary = {"timestamp": stamp, "engine_state": engine["label"], "total": len(results), "results": results}
    (outdir / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(outdir)
    print(f"engine_state: {engine['label']}")
    print(f"result.json files: {len(results)}")
    print("results: " + ", ".join(sorted(set(r["result"] for r in results))))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
