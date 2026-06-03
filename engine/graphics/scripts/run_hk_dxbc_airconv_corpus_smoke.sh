#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="${PROJECT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
source "$PROJECT_ROOT/config/env.sh"
source "$PROJECT_ROOT/engine/graphics/build-support/common.sh"

ARCH="${1:-aarch64}"
BUILD_DIR="${DXMT_SHADER_CORPUS_BUILD_DIR:-$GRAPHICS_BUILD/dxmt-${ARCH}-tests}"
AIRCONV_TARGET="src/airconv/darwin/airconv"
AIRCONV="$BUILD_DIR/$AIRCONV_TARGET"
HK_DATA="${HK_DXBC_DATA_DIR:-$PROJECT_ROOT/../game-hollow.knight-(89718)/extracted-hollow-knight-1.5.12620/Hollow Knight_Data}"
LIMIT="${HK_DXBC_CORPUS_LIMIT:-24}"
LOG_DIR="$PROJECT_ROOT/artifacts/hk-dxbc-corpus"
STAMP="$(date +%Y%m%d-%H%M%S)"
RUN_DIR="$LOG_DIR/run-$STAMP"
BLOB_DIR="$RUN_DIR/blobs"

mkdir -p "$BLOB_DIR"
"$PROJECT_ROOT/scripts/disk-guard.sh" --check-only >/dev/null

if [[ ! -d "$HK_DATA" ]]; then
  echo "hk_dxbc_corpus_result=FAIL reason=missing_hk_data path=$HK_DATA"
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
echo "hk_data=$HK_DATA"

python3 - "$HK_DATA" "$BLOB_DIR" "$LIMIT" >"$RUN_DIR/extract.log" <<'PY'
from pathlib import Path
import hashlib
import struct
import sys

root = Path(sys.argv[1])
out = Path(sys.argv[2])
limit = int(sys.argv[3])

candidates = [
    root / "resources.assets",
    root / "sharedassets0.assets",
    root / "globalgamemanagers.assets",
    root / "Resources" / "unity_builtin_extra",
    root / "Resources" / "unity default resources",
]

seen = set()
wrote = 0
scanned = 0
manifest = []
stats = {str(path): {"magic": 0, "valid": 0, "emitted": 0} for path in candidates}

for path in candidates:
    if not path.is_file():
        continue
    data = path.read_bytes()
    pos = 0
    while wrote < limit:
        off = data.find(b"DXBC", pos)
        if off < 0:
            break
        pos = off + 4
        scanned += 1
        stats[str(path)]["magic"] += 1
        if off + 32 > len(data):
            continue
        size = struct.unpack_from("<I", data, off + 24)[0]
        if size < 32 or size > 2_000_000 or off + size > len(data):
            continue
        stats[str(path)]["valid"] += 1
        blob = data[off:off + size]
        digest = hashlib.sha256(blob).hexdigest()
        key = digest[:16]
        if key in seen:
            continue
        seen.add(key)
        safe_source = path.name.replace(" ", "_")
        name = f"{wrote:03d}_{safe_source}_{key}.dxbc"
        (out / name).write_bytes(blob)
        manifest.append(f"{name}\t{size}\t{digest}\t{path}\t{off}")
        stats[str(path)]["emitted"] += 1
        wrote += 1
        if wrote >= limit:
            break

(out.parent / "manifest.tsv").write_text("\n".join(manifest) + ("\n" if manifest else ""))
print(f"extract_dir={out}")
print(f"extracted={wrote} scanned_magic={scanned}")
for source, source_stats in stats.items():
    print(
        "source_stats "
        f"magic={source_stats['magic']} "
        f"valid={source_stats['valid']} "
        f"emitted={source_stats['emitted']} "
        f"path={source}"
    )
PY

grep -E "^(extract_dir=|extracted=|source_stats )" "$RUN_DIR/extract.log"
blob_count="$(find "$BLOB_DIR" -maxdepth 1 -type f -name '*.dxbc' | wc -l | tr -d ' ')"
echo "dxbc_blobs=$blob_count"

pass_count=0
fail_count=0
shopt -s nullglob
for blob in "$BLOB_DIR"/*.dxbc; do
  base="$(basename "$blob" .dxbc)"
  if "$AIRCONV" -S -o "$RUN_DIR/$base.ll" "$blob" >"$RUN_DIR/$base.log" 2>&1; then
    pass_count=$((pass_count + 1))
    echo "PASS $(basename "$blob")"
  else
    fail_count=$((fail_count + 1))
    echo "FAIL $(basename "$blob")"
    grep -E "error:|assert|Unsupported|Unexpected|unimplemented|Cannot|Failed" \
      "$RUN_DIR/$base.log" | head -5 || true
  fi
done
shopt -u nullglob

echo "run_dir=$RUN_DIR"
echo "hk_dxbc_corpus_pass=$pass_count"
echo "hk_dxbc_corpus_fail=$fail_count"

if [[ "$pass_count" -eq 0 || "$fail_count" -ne 0 ]]; then
  echo "hk_dxbc_corpus_result=FAIL"
  exit 1
fi

echo "hk_dxbc_corpus_result=PASS"
