#!/usr/bin/env python3
import argparse
import datetime as dt
import json
import os
import pathlib
import sqlite3
import sys
from typing import Any

import yaml

COMPAT_HOME = pathlib.Path(os.environ.get("MACRUNNER_COMPAT_HOME", pathlib.Path.home() / ".macrunner-compat"))
REPO = COMPAT_HOME / "winget-pkgs"
INDEX = COMPAT_HOME / "index.sqlite"


def yaml_load(path: pathlib.Path) -> Any:
    with path.open("r", encoding="utf-8") as f:
        return yaml.safe_load(f) or {}


def package_path(winget_id: str) -> pathlib.Path:
    parts = winget_id.split(".")
    if len(parts) < 2:
        raise ValueError(f"invalid winget id: {winget_id}")
    vendor = parts[0].lower()
    rest = [p.lower() for p in parts[1:]]
    return REPO / "manifests" / vendor[0] / vendor / pathlib.Path(*rest)


def version_key(value: str):
    out = []
    for part in value.replace("-", ".").replace("_", ".").split("."):
        try:
            out.append((0, int(part)))
        except ValueError:
            out.append((1, part.lower()))
    return out


def latest_version_dir(pkg: pathlib.Path) -> pathlib.Path:
    versions = [p for p in pkg.iterdir() if p.is_dir()]
    if not versions:
        raise FileNotFoundError(f"no versions under {pkg}")
    return sorted(versions, key=lambda p: version_key(p.name))[-1]


def manifest_candidates(winget_id: str) -> list[pathlib.Path]:
    manifests = REPO / "manifests"
    if not manifests.exists():
        return []
    return list(manifests.rglob(f"{winget_id}.installer.yaml"))


def pick_installer(installers: list[dict[str, Any]]) -> dict[str, Any] | None:
    candidates = []
    for inst in installers:
        arch = str(inst.get("Architecture", "")).lower()
        url = str(inst.get("InstallerUrl", ""))
        sha = str(inst.get("InstallerSha256", ""))
        if not url or len(sha) != 64:
            continue
        score = 0
        if arch in {"x64", "neutral"}:
            score += 10
        if url.lower().endswith((".exe", ".msi")):
            score += 5
        if str(inst.get("Scope", "")).lower() == "user":
            score += 1
        candidates.append((score, inst))
    if not candidates:
        return None
    return sorted(candidates, key=lambda x: x[0], reverse=True)[0][1]


def with_manifest_defaults(data: dict[str, Any], installer: dict[str, Any]) -> dict[str, Any]:
    merged = dict(installer)
    for key in ("InstallerType", "Scope", "InstallerSwitches"):
        if key not in merged and data.get(key) is not None:
            merged[key] = data.get(key)
    return merged


def default_silent_args(installer_type: str) -> str:
    kind = installer_type.lower()
    if kind in {"nullsoft", "nsis"}:
        return "/S"
    if kind in {"inno", "inno setup"}:
        return "/VERYSILENT /NORESTART"
    if kind in {"msi", "wix", "burn"}:
        return "/quiet /norestart"
    return ""


def from_files(winget_id: str, version: str | None = None) -> dict[str, Any]:
    manifests = manifest_candidates(winget_id)
    if version:
        manifests = [m for m in manifests if m.parent.name == version]
    if not manifests:
        # Keep the old deterministic path as a final fallback for hand-written IDs.
        pkg = package_path(winget_id)
        version_dir = pkg / version if version else latest_version_dir(pkg)
        manifests = sorted(version_dir.glob("*.installer.yaml"))
    if not manifests:
        raise FileNotFoundError(f"no installer manifest for {winget_id}")
    manifest = sorted(manifests, key=lambda p: version_key(p.parent.name))[-1]
    data = yaml_load(manifest)
    installer = pick_installer(data.get("Installers", []))
    if not installer:
        raise ValueError(f"no supported installer for {winget_id} {manifest.parent.name}")
    resolved_id = str(data.get("PackageIdentifier") or winget_id)
    resolved_version = str(data.get("PackageVersion") or manifest.parent.name)
    return normalize(resolved_id, resolved_version, with_manifest_defaults(data, installer), manifest)


def from_index(winget_id: str) -> dict[str, Any] | None:
    if not INDEX.exists():
        return None
    con = sqlite3.connect(INDEX)
    con.row_factory = sqlite3.Row
    row = con.execute("SELECT * FROM packages WHERE winget_id=? LIMIT 1", (winget_id,)).fetchone()
    con.close()
    if not row:
        return None
    return {k: row[k] for k in row.keys()}


def normalize(winget_id: str, version: str, installer: dict[str, Any], source: pathlib.Path) -> dict[str, Any]:
    switches = installer.get("InstallerSwitches") or {}
    installer_type = str(installer.get("InstallerType") or "unknown")
    silent = switches.get("Silent") or switches.get("SilentWithProgress") or default_silent_args(installer_type)
    source_date = dt.datetime.fromtimestamp(source.stat().st_mtime, dt.timezone.utc).isoformat()
    return {
        "winget_id": winget_id,
        "latest_version": version,
        "version": version,
        "installer_url": installer.get("InstallerUrl"),
        "url": installer.get("InstallerUrl"),
        "installer_sha256": str(installer.get("InstallerSha256", "")).lower(),
        "sha256": str(installer.get("InstallerSha256", "")).lower(),
        "silent_args": silent or "",
        "args": silent or "",
        "scope": installer.get("Scope") or "unknown",
        "architecture": installer.get("Architecture") or "unknown",
        "installer_type": installer_type,
        "manifest_path": str(source),
        "manifest_source_date": source_date,
    }


def resolve(winget_id: str, version: str | None = None) -> dict[str, Any]:
    if version is None:
        indexed = from_index(winget_id)
        if indexed:
            indexed["version"] = indexed.get("latest_version")
            indexed["url"] = indexed.get("installer_url")
            indexed["sha256"] = indexed.get("installer_sha256")
            indexed["args"] = indexed.get("silent_args")
            return indexed
    return from_files(winget_id, version)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("winget_id")
    ap.add_argument("--version")
    args = ap.parse_args()
    try:
        print(json.dumps(resolve(args.winget_id, args.version), indent=2, sort_keys=True))
        return 0
    except Exception as exc:
        print(f"manifest_reader error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
