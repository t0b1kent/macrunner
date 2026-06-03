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
LIMIT="${HK_UNITY_SHADER_DXBC_LIMIT:-128}"
LOG_DIR="$PROJECT_ROOT/artifacts/hk-unity-shader-dxbc"
STAMP="$(date +%Y%m%d-%H%M%S)"
RUN_DIR="$LOG_DIR/run-$STAMP"
BLOB_DIR="$RUN_DIR/blobs"

mkdir -p "$BLOB_DIR"
"$PROJECT_ROOT/scripts/disk-guard.sh" --check-only >/dev/null

if [[ ! -d "$HK_DATA" ]]; then
  echo "hk_unity_shader_dxbc_result=FAIL reason=missing_hk_data path=$HK_DATA"
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
import json
import struct
import sys

root = Path(sys.argv[1])
out = Path(sys.argv[2])
limit = int(sys.argv[3])

candidates = [
    root / "resources.assets",
    root / "sharedassets0.assets",
    root / "globalgamemanagers.assets",
]

def u32(data, off):
    return struct.unpack_from("<I", data, off)[0]

def i32(data, off):
    return struct.unpack_from("<i", data, off)[0]

def i16(data, off):
    return struct.unpack_from("<h", data, off)[0]

def i64(data, off):
    return struct.unpack_from("<q", data, off)[0]

def align4(pos):
    return (pos + 3) & ~3

def parse_serialized_file(path):
    data = path.read_bytes()
    if len(data) < 64 or struct.unpack_from(">I", data, 8)[0] != 22:
        return data, []

    data_offset = struct.unpack_from(">Q", data, 32)[0]
    pos = data.index(b"\0", 48) + 1
    pos += 4  # target platform
    pos += 1  # enable type tree
    type_count = i32(data, pos)
    pos += 4

    classes = []
    for _ in range(type_count):
        class_id = i32(data, pos)
        pos += 4
        pos += 1  # stripped
        pos += 2  # script type index
        if class_id == 114 or class_id < 0:
            pos += 16
        pos += 16
        classes.append(class_id)

    object_count = i32(data, pos)
    pos += 4
    objects = []
    for _ in range(object_count):
        pos = align4(pos)
        path_id = i64(data, pos)
        pos += 8
        start = struct.unpack_from("<Q", data, pos)[0]
        pos += 8
        size = u32(data, pos)
        pos += 4
        type_id = i32(data, pos)
        pos += 4
        class_id = classes[type_id] if 0 <= type_id < len(classes) else None
        objects.append((path_id, data_offset + start, size, type_id, class_id))
    return data, objects

def parse_int_nested_array(data, pos, expected_outer):
    outer = i32(data, pos)
    pos += 4
    if outer != expected_outer:
        raise ValueError("unexpected outer array size")
    values = []
    for _ in range(outer):
        inner = i32(data, pos)
        pos += 4
        if inner <= 0 or inner > 16:
            raise ValueError("unexpected inner array size")
        row = []
        for _ in range(inner):
            row.append(i32(data, pos))
            pos += 4
        values.append(row)
    return values, pos

def parse_blob_metadata(payload, blob_start, blob_len):
    scan_start = max(0, blob_start - 2048)
    for pos in range(scan_start, blob_start - 4):
        try:
            cursor = pos
            platform_count = i32(payload, cursor)
            cursor += 4
            if platform_count <= 0 or platform_count > 32:
                continue
            platforms = [i32(payload, cursor + 4 * i) for i in range(platform_count)]
            cursor += 4 * platform_count
            if any(platform < 0 or platform > 64 for platform in platforms):
                continue
            offsets, cursor = parse_int_nested_array(payload, cursor, platform_count)
            compressed_lengths, cursor = parse_int_nested_array(payload, cursor, platform_count)
            decompressed_lengths, cursor = parse_int_nested_array(payload, cursor, platform_count)
            if cursor + 4 != blob_start:
                continue
            if i32(payload, cursor) != blob_len:
                continue
            for rows in (offsets, compressed_lengths, decompressed_lengths):
                if any(len(row) != 1 for row in rows):
                    raise ValueError("tiered shader blob arrays are not supported")
            return {
                "platforms": platforms,
                "offsets": [row[0] for row in offsets],
                "compressed_lengths": [row[0] for row in compressed_lengths],
                "decompressed_lengths": [row[0] for row in decompressed_lengths],
            }
        except (struct.error, ValueError):
            continue
    return None

def lz4_block_decompress(src, expected_size):
    out_buf = bytearray()
    ip = 0
    while ip < len(src):
        token = src[ip]
        ip += 1

        literal_len = token >> 4
        if literal_len == 15:
            while True:
                b = src[ip]
                ip += 1
                literal_len += b
                if b != 255:
                    break
        out_buf.extend(src[ip:ip + literal_len])
        ip += literal_len
        if ip >= len(src):
            break

        match_offset = src[ip] | (src[ip + 1] << 8)
        ip += 2
        if match_offset == 0 or match_offset > len(out_buf):
            raise ValueError("invalid lz4 match offset")

        match_len = token & 0x0f
        if match_len == 15:
            while True:
                b = src[ip]
                ip += 1
                match_len += b
                if b != 255:
                    break
        match_len += 4
        for _ in range(match_len):
            out_buf.append(out_buf[-match_offset])

    if len(out_buf) != expected_size:
        raise ValueError(f"lz4 size mismatch got={len(out_buf)} expected={expected_size}")
    return bytes(out_buf)

def read_aligned_string(data, pos):
    size = i32(data, pos)
    pos += 4
    if size < 0 or pos + size > len(data):
        raise ValueError("bad string size")
    raw = data[pos:pos + size]
    pos = align4(pos + size)
    return raw.rstrip(b"\0").decode("utf-8", "replace"), pos

def read_byte_array(data, pos):
    size = i32(data, pos)
    pos += 4
    if size < 0 or pos + size > len(data):
        raise ValueError("bad byte array size")
    raw = data[pos:pos + size]
    pos = align4(pos + size)
    return raw, pos

def parse_shader_program(data):
    if len(data) < 4:
        return []
    capacity = i32(data, 0)
    if capacity < 0 or capacity > 4096:
        return []
    entry_size = 12
    subprograms = []
    for index in range(capacity):
        entry = 4 + index * entry_size
        if entry + 4 > len(data):
            break
        offset = i32(data, entry)
        if offset <= 0 or offset >= len(data):
            continue
        try:
            cursor = offset
            version = i32(data, cursor)
            cursor += 4
            program_type = i32(data, cursor)
            cursor += 4
            cursor += 12
            if version >= 201608170:
                cursor += 4
            keyword_count = i32(data, cursor)
            cursor += 4
            if keyword_count < 0 or keyword_count > 4096:
                continue
            for _ in range(keyword_count):
                _, cursor = read_aligned_string(data, cursor)
            if 201806140 <= version < 202012090:
                local_keyword_count = i32(data, cursor)
                cursor += 4
                if local_keyword_count < 0 or local_keyword_count > 4096:
                    continue
                for _ in range(local_keyword_count):
                    _, cursor = read_aligned_string(data, cursor)
            code, _ = read_byte_array(data, cursor)
            subprograms.append((index, version, program_type, code))
        except (struct.error, ValueError):
            continue
    return subprograms

stats = {
    "files": 0,
    "objects": 0,
    "shader_objects": 0,
    "shader_blobs": 0,
    "d3d11_blocks": 0,
    "subprograms": 0,
    "dxbc_found": 0,
    "dxbc_emitted": 0,
    "duplicates": 0,
    "parse_failures": 0,
}
asset_stats = []
seen = set()
manifest = []

for path in candidates:
    if not path.is_file() or stats["dxbc_emitted"] >= limit:
        continue
    file_stats = {
        "path": str(path),
        "objects": 0,
        "shader_objects": 0,
        "shader_blobs": 0,
        "d3d11_blocks": 0,
        "subprograms": 0,
        "dxbc_found": 0,
        "dxbc_emitted": 0,
        "parse_failures": 0,
    }
    try:
        data, objects = parse_serialized_file(path)
    except Exception:
        stats["parse_failures"] += 1
        file_stats["parse_failures"] += 1
        asset_stats.append(file_stats)
        continue

    stats["files"] += 1
    stats["objects"] += len(objects)
    file_stats["objects"] = len(objects)

    for path_id, abs_start, size, type_id, class_id in objects:
        if stats["dxbc_emitted"] >= limit:
            break
        if class_id != 48:
            continue
        stats["shader_objects"] += 1
        file_stats["shader_objects"] += 1
        payload = data[abs_start:abs_start + size]

        blob_candidates = []
        cursor = 4
        while cursor + 4 < len(payload):
            blob_len = i32(payload, cursor - 4)
            if 0 < blob_len <= len(payload) - cursor:
                blob = payload[cursor:cursor + blob_len]
                if b"DXBC" in blob:
                    meta = parse_blob_metadata(payload, cursor, blob_len)
                    if meta:
                        blob_candidates.append((cursor, blob, meta))
            cursor += 4

        if not blob_candidates:
            continue
        blob_start, compressed_blob, meta = blob_candidates[-1]
        stats["shader_blobs"] += 1
        file_stats["shader_blobs"] += 1

        for platform, block_offset, compressed_len, decompressed_len in zip(
            meta["platforms"],
            meta["offsets"],
            meta["compressed_lengths"],
            meta["decompressed_lengths"],
        ):
            if platform != 4:
                continue
            if stats["dxbc_emitted"] >= limit:
                break
            if block_offset < 0 or compressed_len <= 0 or decompressed_len <= 0:
                continue
            block = compressed_blob[block_offset:block_offset + compressed_len]
            if len(block) != compressed_len:
                continue
            try:
                decompressed = lz4_block_decompress(block, decompressed_len)
            except ValueError:
                stats["parse_failures"] += 1
                file_stats["parse_failures"] += 1
                continue
            stats["d3d11_blocks"] += 1
            file_stats["d3d11_blocks"] += 1
            subprograms = parse_shader_program(decompressed)
            stats["subprograms"] += len(subprograms)
            file_stats["subprograms"] += len(subprograms)
            for sub_index, version, program_type, code in subprograms:
                if stats["dxbc_emitted"] >= limit:
                    break
                dxbc_offset = code.find(b"DXBC")
                if dxbc_offset < 0:
                    continue
                dxbc = code[dxbc_offset:]
                if len(dxbc) < 32:
                    continue
                dxbc_size = u32(dxbc, 24)
                if dxbc_size < 32 or dxbc_size > len(dxbc):
                    continue
                dxbc = dxbc[:dxbc_size]
                digest = hashlib.sha256(dxbc).hexdigest()
                stats["dxbc_found"] += 1
                file_stats["dxbc_found"] += 1
                if digest in seen:
                    stats["duplicates"] += 1
                    continue
                seen.add(digest)
                safe_source = path.name.replace(" ", "_")
                name = (
                    f"{stats['dxbc_emitted']:04d}_{safe_source}_"
                    f"path{path_id}_sp{sub_index}_pt{program_type}_{digest[:16]}.dxbc"
                )
                (out / name).write_bytes(dxbc)
                manifest.append(
                    "\t".join(
                        [
                            name,
                            str(dxbc_size),
                            digest,
                            str(path),
                            str(path_id),
                            str(platform),
                            str(sub_index),
                            str(version),
                            str(program_type),
                        ]
                    )
                )
                stats["dxbc_emitted"] += 1
                file_stats["dxbc_emitted"] += 1

    asset_stats.append(file_stats)

(out.parent / "manifest.tsv").write_text(
    "name\tsize\tsha256\tsource\tpath_id\tplatform\tsubprogram\tversion\tprogram_type\n"
    + "\n".join(manifest)
    + ("\n" if manifest else "")
)

print(f"extract_dir={out}")
for key in sorted(stats):
    print(f"extract_{key}={stats[key]}")
for item in asset_stats:
    print("asset_stats " + json.dumps(item, sort_keys=True))
PY

grep -E "^(extract_dir=|extract_|asset_stats )" "$RUN_DIR/extract.log"
blob_count="$(find "$BLOB_DIR" -maxdepth 1 -type f -name '*.dxbc' | wc -l | tr -d ' ')"
echo "dxbc_blobs=$blob_count"

pass_count=0
fail_count=0
shopt -s nullglob
for blob in "$BLOB_DIR"/*.dxbc; do
  base="$(basename "$blob" .dxbc)"
  if "$AIRCONV" -S -o "$RUN_DIR/$base.ll" "$blob" >"$RUN_DIR/$base.log" 2>&1; then
    pass_count=$((pass_count + 1))
  else
    fail_count=$((fail_count + 1))
    echo "FAIL $(basename "$blob")"
    grep -E "error:|assert|Unsupported|Unexpected|unimplemented|Cannot|Failed|Invalid DXBC" \
      "$RUN_DIR/$base.log" | head -5 || true
  fi
done
shopt -u nullglob

echo "run_dir=$RUN_DIR"
echo "hk_unity_shader_dxbc_pass=$pass_count"
echo "hk_unity_shader_dxbc_fail=$fail_count"

if [[ "$pass_count" -eq 0 || "$fail_count" -ne 0 ]]; then
  echo "hk_unity_shader_dxbc_result=FAIL"
  exit 1
fi

echo "hk_unity_shader_dxbc_result=PASS"
