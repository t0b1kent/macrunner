#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="${PROJECT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
source "$PROJECT_ROOT/config/env.sh"
source "$PROJECT_ROOT/engine/graphics/build-support/common.sh"

ARCH="${1:-aarch64}"
BUILD_DIR="${DXMT_SHADER_CORPUS_BUILD_DIR:-$GRAPHICS_BUILD/dxmt-${ARCH}-tests}"
AIRCONV_TARGET="src/airconv/darwin/airconv"
AIRCONV="$BUILD_DIR/$AIRCONV_TARGET"
UNITY_DATA_DIR="${UNITY_DXBC_DATA_DIR:-$PROJECT_ROOT/artifacts/ai-war2-unity-corpus/extracted/AIWar2_Data}"
CORPUS_NAME="${UNITY_DXBC_CORPUS_NAME:-ai-war2}"
LIMIT="${UNITY_DXBC_CORPUS_LIMIT:-0}"
LOG_DIR="$PROJECT_ROOT/artifacts/unity-dxbc-airconv-corpus/$CORPUS_NAME"
STAMP="$(date +%Y%m%d-%H%M%S)"
RUN_DIR="$LOG_DIR/run-$STAMP"
BLOB_DIR="$RUN_DIR/blobs"

mkdir -p "$BLOB_DIR"
"$PROJECT_ROOT/scripts/disk-guard.sh" --check-only >/dev/null

if [[ ! -d "$UNITY_DATA_DIR" ]]; then
  echo "unity_dxbc_airconv_result=FAIL reason=missing_unity_data path=$UNITY_DATA_DIR"
  exit 2
fi

if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
  SMOKE_REPEAT_COUNT=1 SMOKE_STABILITY_FRAMES=1 \
    "$PROJECT_ROOT/engine/graphics/scripts/run_dxmt_d3d11_headless_smoke.sh" "$ARCH" \
    >"$RUN_DIR/bootstrap-headless-smoke.log" 2>&1
fi

ninja -C "$BUILD_DIR" "$AIRCONV_TARGET" >"$RUN_DIR/build-airconv.log" 2>&1

echo "arch=$ARCH"
echo "airconv=$AIRCONV"
echo "unity_data=$UNITY_DATA_DIR"
echo "corpus_name=$CORPUS_NAME"

python3 - "$UNITY_DATA_DIR" "$BLOB_DIR" "$LIMIT" >"$RUN_DIR/extract.log" <<'PY'
from pathlib import Path
import hashlib
import json
import struct
import sys


MAX_SIZE = 16_000_000
MAX_DEPTH = 6

root = Path(sys.argv[1])
out = Path(sys.argv[2])
limit = int(sys.argv[3])

try:
    import UnityPy  # type: ignore
    UNITYPY_AVAILABLE = True
except Exception as exc:
    UnityPy = None
    UNITYPY_AVAILABLE = False
    UNITYPY_ERROR = str(exc)


def valid_dxbc(data, off, size):
    if size < 32 or size > MAX_SIZE or off + size > len(data):
        return False
    chunk_count = struct.unpack_from("<I", data, off + 28)[0]
    if chunk_count <= 0 or chunk_count > 128:
        return False
    table_end = 32 + chunk_count * 4
    if table_end > size:
        return False
    for index in range(chunk_count):
        chunk_off = struct.unpack_from("<I", data, off + 32 + index * 4)[0]
        if chunk_off + 8 > size:
            return False
        fourcc = data[off + chunk_off:off + chunk_off + 4]
        if len(fourcc) != 4 or any(byte < 0x20 or byte > 0x7e for byte in fourcc):
            return False
    return True


def iter_dxbc_blobs_from_bytes(data: bytes):
    pos = 0
    while True:
        off = data.find(b"DXBC", pos)
        if off < 0:
            break
        pos = off + 4
        if off + 32 > len(data):
            continue
        size = struct.unpack_from("<I", data, off + 24)[0]
        if not valid_dxbc(data, off, size):
            continue
        yield off, size, data[off : off + size]


def iter_data_dirs(base: Path):
    data_dirs = set()
    if base.is_dir() and base.name.endswith("_Data"):
        data_dirs.add(base.resolve())
    for child in base.rglob("*_Data"):
        if child.is_dir():
            data_dirs.add(child.resolve())
    return sorted(data_dirs)


def iter_data_files(data_dirs):
    files = []
    seen = set()
    for data_dir in data_dirs:
        for path in data_dir.rglob("*"):
            if not path.is_file():
                continue
            resolved = path.resolve()
            if resolved in seen:
                continue
            seen.add(resolved)
            files.append(path)
    return sorted(files)


def is_unity_container(path: Path):
    name = path.name.lower()
    return (
        path.suffix.lower() in {".assets", ".asset", ".resource", ".resS", ".bundle", ".unity3d"}
        or name.startswith("globalgamemanagers")
        or "sharedassets" in name
        or "unity_builtin" in name
    )


def walk_bytes(payload, depth=0, visited=None):
    if visited is None:
        visited = set()
    if id(payload) in visited:
        return []
    if depth > MAX_DEPTH:
        return []
    if isinstance(payload, (bytes, bytearray, memoryview)):
        return [bytes(payload)]
    visited.add(id(payload))
    blocks = []
    if isinstance(payload, dict):
        for value in payload.values():
            blocks.extend(walk_bytes(value, depth + 1, visited))
        return blocks
    if isinstance(payload, (list, tuple, set)):
        for value in payload:
            blocks.extend(walk_bytes(value, depth + 1, visited))
        return blocks
    if hasattr(payload, "__dict__"):
        for value in vars(payload).values():
            blocks.extend(walk_bytes(value, depth + 1, visited))
    return blocks


def iter_unitypy_blobs(path: Path):
    if not UNITYPY_AVAILABLE or not is_unity_container(path):
        return []

    try:
        env = UnityPy.load(str(path))
    except Exception:
        return []

    for obj in env.objects:
        obj_type = str(getattr(obj, "type", "")).lower()
        if "shader" not in obj_type:
            continue

        try:
            obj_data = obj.read()
        except Exception:
            continue

        for i, payload in enumerate(walk_bytes(obj_data)):
            for off, size, blob in iter_dxbc_blobs_from_bytes(payload):
                yield obj_type, f"{obj_type}_{i}", off, size, blob


data_dirs = iter_data_dirs(root)
candidates = iter_data_files(data_dirs)

seen_blobs = set()
manifest = []
stats = {}
wrote = 0
unitypy_scanned = 0

for path in candidates:
    if limit and wrote >= limit:
        break
    rel = str(path.relative_to(root))
    stats[rel] = {
        "magic": 0,
        "valid": 0,
        "emitted": 0,
        "unitypy_scanned": 0,
        "unitypy_emitted": 0,
    }

    data = path.read_bytes()
    for off, size, blob in iter_dxbc_blobs_from_bytes(data):
        if limit and wrote >= limit:
            break
        stats[rel]["magic"] += 1
        stats[rel]["valid"] += 1
        digest = hashlib.sha1(blob).hexdigest()
        if digest in seen_blobs:
            continue
        seen_blobs.add(digest)
        name = f"{wrote:04d}_{path.stem}_{digest[:12]}.dxbc"
        blob_path = out / name
        blob_path.write_bytes(blob)
        manifest.append({
            "file": rel,
            "offset": off,
            "size": size,
            "sha1": digest,
            "blob": str(blob_path.name),
            "source": "raw_scan",
        })
        stats[rel]["emitted"] += 1
        wrote += 1

    if is_unity_container(path):
        if UNITYPY_AVAILABLE:
            unitypy_scanned += 1
        for source, source_path, off, size, blob in iter_unitypy_blobs(path):
            if limit and wrote >= limit:
                break
            stats[rel]["unitypy_scanned"] += 1
            digest = hashlib.sha1(blob).hexdigest()
            if digest in seen_blobs:
                continue
            seen_blobs.add(digest)
            name = f"{wrote:04d}_{path.stem}_{digest[:12]}.dxbc"
            blob_path = out / name
            blob_path.write_bytes(blob)
            manifest.append({
                "file": rel,
                "offset": off,
                "size": size,
                "sha1": digest,
                "blob": str(blob_path.name),
                "source": "unitypy",
                "unitypy_object": source_path,
                "unitypy_object_type": source,
            })
            stats[rel]["unitypy_emitted"] += 1
            stats[rel]["emitted"] += 1
            wrote += 1

(out.parent / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
(out.parent / "extract-stats.json").write_text(json.dumps(stats, indent=2) + "\n")
print(f"candidate_data_dirs={len(data_dirs)}")
print(f"candidate_files={len(candidates)}")
print(f"unitypy_available={UNITYPY_AVAILABLE}")
if not UNITYPY_AVAILABLE:
    print(f"unitypy_error={UNITYPY_ERROR}")
print(f"unitypy_scanned_files={unitypy_scanned}")
print(f"dxbc_blobs={len(manifest)}")
for rel, item in stats.items():
    if item["magic"] or item["valid"] or item["emitted"] or item["unitypy_scanned"] or item["unitypy_emitted"]:
        print(
            "asset_stats "
            f"file={rel} "
            f"magic={item['magic']} "
            f"valid={item['valid']} "
            f"emitted={item['emitted']} "
            f"unitypy_scanned={item['unitypy_scanned']} "
            f"unitypy_emitted={item['unitypy_emitted']}"
        )
PY

cat "$RUN_DIR/extract.log"

blob_count="$(find "$BLOB_DIR" -name '*.dxbc' -type f | wc -l | tr -d ' ')"
if [[ "$blob_count" == "0" ]]; then
  echo "unity_dxbc_airconv_result=FAIL reason=no_dxbc_blobs run_dir=$RUN_DIR"
  exit 3
fi

translate_pass=0
translate_fail=0
render_pass=0
render_fail=0

while IFS= read -r blob; do
  base="$(basename "$blob" .dxbc)"
  if "$AIRCONV" -S -o "$RUN_DIR/$base.ll" "$blob" >"$RUN_DIR/$base.translate.log" 2>&1; then
    translate_pass=$((translate_pass + 1))
  else
    translate_fail=$((translate_fail + 1))
    echo "$blob" >>"$RUN_DIR/translate-failures.txt"
  fi

  if "$AIRCONV" -A -o "$RUN_DIR/$base.metallib" "$blob" >"$RUN_DIR/$base.render.log" 2>&1; then
    render_pass=$((render_pass + 1))
  else
    render_fail=$((render_fail + 1))
    echo "$blob" >>"$RUN_DIR/render-failures.txt"
  fi
done < <(find "$BLOB_DIR" -name '*.dxbc' -type f | sort)

if [[ "$translate_fail" == "0" && "$render_fail" == "0" ]]; then
  result="PASS"
else
  result="FAIL"
fi

echo "unity_dxbc_airconv_result=$result corpus=$CORPUS_NAME blobs=$blob_count translate=$translate_pass/$blob_count render=$render_pass/$blob_count run_dir=$RUN_DIR"

if [[ "$result" != "PASS" ]]; then
  exit 1
fi
