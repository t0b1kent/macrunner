#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat


WINDOWS_KEY = r"[HKEY_CURRENT_USER\Software\Team Cherry\Hollow Knight]"
WINE_KEY = r"[Software\\Team Cherry\\Hollow Knight]"


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def logical_reg_lines(path):
    text = path.read_bytes().decode("utf-16")
    logical = []
    pending = ""
    for physical in text.splitlines():
        line = physical.strip()
        pending += line
        if pending.endswith("\\"):
            pending = pending[:-1]
            continue
        logical.append(pending)
        pending = ""
    if pending:
        raise RuntimeError("unterminated registry continuation")
    return logical


def extract_values(path):
    values = []
    in_key = False
    for line in logical_reg_lines(path):
        if line.startswith("["):
            in_key = line.casefold() == WINDOWS_KEY.casefold()
            continue
        if in_key and line.startswith('"') and "=" in line:
            values.append(line)
    if not values:
        raise RuntimeError("Hollow Knight registry key has no values")
    if len({line.split('"=', 1)[0] for line in values}) != len(values):
        raise RuntimeError("duplicate registry value names")
    return values


def verify_oracle(oracle):
    manifest_path = oracle / "PAYLOAD-MANIFEST.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    payload = oracle / "payload"
    seen = set()
    for entry in manifest["files"]:
        relative = Path(entry["path"])
        if relative.is_absolute() or ".." in relative.parts:
            raise RuntimeError(f"unsafe payload path: {relative}")
        path = payload / relative
        if not path.is_file():
            raise RuntimeError(f"missing payload file: {relative}")
        if path.stat().st_size != int(entry["size"]) or sha256(path) != entry["sha256"]:
            raise RuntimeError(f"payload identity mismatch: {relative}")
        seen.add(relative.as_posix())
    actual = {path.relative_to(payload).as_posix() for path in payload.rglob("*") if path.is_file()}
    if actual != seen:
        raise RuntimeError("payload inventory mismatch")
    return manifest


def make_writable(root):
    for path in [root, *root.rglob("*")]:
        if path.is_symlink():
            continue
        mode = path.stat().st_mode
        path.chmod(mode | stat.S_IWUSR)


def replace_wine_key(user_reg, values):
    lines = user_reg.read_text(encoding="utf-8").splitlines()
    start = next((i for i, line in enumerate(lines) if line.startswith(WINE_KEY + " ")), None)
    if start is None:
        raise RuntimeError("base prefix lacks Hollow Knight registry section")
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("[")), len(lines))
    metadata = [line for line in lines[start + 1:end] if line.startswith("#time=")]
    replacement = [lines[start], *(metadata[:1]), *values, ""]
    updated = lines[:start] + replacement + lines[end:]
    user_reg.write_text("\n".join(updated) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--oracle", type=Path, required=True)
    parser.add_argument("--base-prefix", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    oracle = args.oracle.resolve()
    base = args.base_prefix.resolve()
    output = args.output.resolve()
    if output.exists():
        raise RuntimeError(f"refusing existing output: {output}")

    manifest = verify_oracle(oracle)
    reg_path = oracle / "payload" / "Hollow-Knight-HKCU.reg"
    values = extract_values(reg_path)
    if len(values) != 40:
        raise RuntimeError(f"expected 40 registry values, found {len(values)}")

    shutil.copytree(base, output, symlinks=True, copy_function=shutil.copy2)
    make_writable(output)
    save_source = oracle / "payload" / "LocalLow" / "Team Cherry" / "Hollow Knight"
    save_target = output / "drive_c" / "users" / "crossover" / "AppData" / "LocalLow" / "Team Cherry" / "Hollow Knight"
    save_target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(save_source, save_target, copy_function=shutil.copy2)
    replace_wine_key(output / "user.reg", values)

    user_text = (output / "user.reg").read_text(encoding="utf-8")
    missing = [line for line in values if line not in user_text]
    if missing:
        raise RuntimeError(f"registry verification failed for {len(missing)} values")

    result = {
        "schema": 1,
        "classification": "WINDOWS_ORACLE_PREFIX_TEMPLATE_NOT_GOLDEN",
        "oracle_run_id": manifest["run_id"],
        "oracle_payload_manifest_sha256": sha256(oracle / "PAYLOAD-MANIFEST.json"),
        "registry_value_count": len(values),
        "save_file_count": sum(path.is_file() for path in save_target.rglob("*")),
        "user_reg_sha256": sha256(output / "user.reg"),
    }
    result_path = output / "WINDOWS-ORACLE-IMPORT.json"
    result_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()
