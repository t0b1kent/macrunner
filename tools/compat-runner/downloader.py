#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
import pathlib
import sys
import tempfile
import urllib.error
import urllib.request

import yaml
from manifest_reader import resolve

COMPAT_HOME = pathlib.Path(os.environ.get("MACRUNNER_COMPAT_HOME", pathlib.Path.home() / ".macrunner-compat"))
CACHE = COMPAT_HOME / "cache"
USER_AGENT = "MacRunner/1.0.2 compat-runner"
USE_SYSTEM_PROXY = os.environ.get("MACRUNNER_COMPAT_USE_SYSTEM_PROXY") == "1"


def sha256_file(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def load_plan(path: pathlib.Path) -> list[dict]:
    data = yaml.safe_load(path.read_text(encoding="utf-8"))
    if isinstance(data, list):
        return data
    return data.get("programs", [])


def suffix_for(url: str) -> str:
    name = pathlib.PurePosixPath(urllib.request.urlparse(url).path).name
    suffix = pathlib.Path(name).suffix
    return suffix or ".bin"


def download_url(url: str, expected_sha: str) -> tuple[pathlib.Path, str]:
    CACHE.mkdir(parents=True, exist_ok=True)
    expected_sha = expected_sha.lower()
    target = CACHE / f"{expected_sha[:8]}{suffix_for(url)}"
    if target.exists() and sha256_file(target) == expected_sha:
        return target, "cached"
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    opener = urllib.request.build_opener() if USE_SYSTEM_PROXY else urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        with opener.open(request, timeout=180) as response:
            status = getattr(response, "status", None)
            if status is not None and (status < 200 or status >= 300):
                raise RuntimeError(f"HTTP {status}: {url}")
            with tempfile.NamedTemporaryFile(delete=False, dir=str(CACHE)) as tmp:
                while True:
                    chunk = response.read(1024 * 1024)
                    if not chunk:
                        break
                    tmp.write(chunk)
                tmp_path = pathlib.Path(tmp.name)
    except urllib.error.HTTPError as exc:
        raise RuntimeError(f"HTTP {exc.code}: {url}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"network error for {url}: {exc.reason}") from exc
    actual = sha256_file(tmp_path)
    if actual != expected_sha:
        tmp_path.unlink(missing_ok=True)
        raise ValueError(f"sha256 mismatch, expected {expected_sha}, got {actual}, refusing")
    tmp_path.replace(target)
    return target, "downloaded"


def resolve_input(args) -> dict:
    if args.winget_id:
        item = resolve(args.winget_id)
        if args.expect_sha256:
            item["sha256"] = args.expect_sha256.lower()
            item["installer_sha256"] = args.expect_sha256.lower()
        return item
    if args.url and args.expect_sha256:
        return {"winget_id": args.url, "installer_url": args.url, "sha256": args.expect_sha256.lower(), "installer_sha256": args.expect_sha256.lower()}
    raise SystemExit("provide --winget-id or --url with --expect-sha256")


def download_item(item: dict) -> tuple[pathlib.Path, str, str]:
    url = item.get("installer_url") or item.get("url")
    expected = (item.get("installer_sha256") or item.get("sha256") or "").lower()
    if not url or len(expected) != 64:
        raise ValueError(f"missing URL/SHA-256 for {item.get('winget_id')}")
    path, state = download_url(url, expected)
    return path, state, expected


def run_plan(path: pathlib.Path, auto_only: bool) -> int:
    entries = load_plan(path)
    auto = [p for p in entries if p.get("winget_id")]
    selected = auto if auto_only else entries
    downloaded = 0
    cached = 0
    manual = len(entries) - len(auto)
    failures = []
    for entry in selected:
        if not entry.get("winget_id"):
            continue
        try:
            item = resolve(entry["winget_id"])
            path, state, digest = download_item(item)
            downloaded += 1
            if state == "cached":
                cached += 1
            print(f"{entry['id']}: {state} {path} sha256={digest}")
        except Exception as exc:
            failures.append(f"{entry.get('id')}: {exc}")
    if failures:
        q = pathlib.Path("docs/AGENT-QUESTIONS.md")
        q.write_text("# Agent Questions - compat winget downloader\n\n" + "\n".join(f"- {f}" for f in failures) + "\n", encoding="utf-8")
        for failure in failures:
            print(f"ERROR: {failure}", file=sys.stderr)
        return 1
    if auto_only:
        print(f"downloaded: {downloaded}/{len(auto)} auto-entries, cached: {cached}/{len(auto)}, manual_required: {manual}/{len(entries)} skipped")
    else:
        print(f"downloaded: {downloaded}/{len(entries)}, cached: {cached}/{len(entries)}, manual_required: {manual}/{len(entries)}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--winget-id")
    ap.add_argument("--url")
    ap.add_argument("--expect-sha256")
    ap.add_argument("--plan")
    ap.add_argument("--auto-only", action="store_true")
    args = ap.parse_args()
    try:
        if args.plan:
            return run_plan(pathlib.Path(args.plan), args.auto_only)
        item = resolve_input(args)
        path, state, digest = download_item(item)
        print(f"{state} to {path} (sha256 verified {digest})")
        return 0
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
