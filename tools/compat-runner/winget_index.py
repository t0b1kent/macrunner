#!/usr/bin/env python3
import os
import pathlib
import sqlite3
import time

import yaml
from manifest_reader import normalize, pick_installer, with_manifest_defaults, version_key

COMPAT_HOME = pathlib.Path(os.environ.get("MACRUNNER_COMPAT_HOME", pathlib.Path.home() / ".macrunner-compat"))
REPO = COMPAT_HOME / "winget-pkgs"
INDEX = COMPAT_HOME / "index.sqlite"


def load_yaml(path: pathlib.Path):
    with path.open("r", encoding="utf-8") as f:
        return yaml.safe_load(f) or {}


def iter_latest_installer_manifests():
    manifests = REPO / "manifests"
    latest: dict[str, tuple[list[tuple[int, int | str]], str, pathlib.Path]] = {}
    suffix = ".installer.yaml"
    for installer in manifests.rglob(f"*{suffix}"):
        winget_id = installer.name.removesuffix(suffix)
        version = installer.parent.name
        key = version_key(version)
        current = latest.get(winget_id)
        if current is None or key > current[0]:
            latest[winget_id] = (key, version, installer)
    for _, version, installer in latest.values():
        yield version, installer


def main() -> int:
    if not (REPO / "manifests").exists():
        print(f"winget-pkgs clone missing: {REPO}")
        return 1
    start = time.time()
    rows = []
    for version, manifest in iter_latest_installer_manifests():
        try:
            data = load_yaml(manifest)
            winget_id = str(data.get("PackageIdentifier") or manifest.name.removesuffix(".installer.yaml"))
            version = str(data.get("PackageVersion") or version)
            installer = pick_installer(data.get("Installers", []))
            if not installer:
                continue
            item = normalize(winget_id, version, with_manifest_defaults(data, installer), manifest)
            rows.append(item)
        except Exception:
            continue
    COMPAT_HOME.mkdir(parents=True, exist_ok=True)
    con = sqlite3.connect(INDEX)
    con.execute("DROP TABLE IF EXISTS packages")
    con.execute(
        "CREATE TABLE packages(winget_id TEXT PRIMARY KEY, latest_version TEXT, installer_url TEXT, installer_sha256 TEXT, silent_args TEXT, scope TEXT, architecture TEXT, installer_type TEXT, manifest_path TEXT, manifest_source_date TEXT)"
    )
    con.executemany(
        "INSERT OR REPLACE INTO packages VALUES(?,?,?,?,?,?,?,?,?,?)",
        [
            (
                r["winget_id"], r["latest_version"], r["installer_url"], r["installer_sha256"], r["silent_args"],
                r["scope"], r["architecture"], r["installer_type"], r["manifest_path"], str(r["manifest_source_date"]),
            )
            for r in rows
        ],
    )
    con.execute("CREATE INDEX idx_packages_url ON packages(installer_url)")
    con.commit()
    con.close()
    elapsed = time.time() - start
    print(f"indexed: {len(rows)} packages in {elapsed:.1f}s -> {INDEX}")
    return 0 if len(rows) >= 5000 else 1


if __name__ == "__main__":
    raise SystemExit(main())
