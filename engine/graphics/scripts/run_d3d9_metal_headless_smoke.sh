#!/usr/bin/env bash
set -euo pipefail

if [[ "${ALLOW_GPU_SMOKE:-0}" != "1" ]]; then
  echo "d3d9_metal_headless_smoke=SKIP reason=ALLOW_GPU_SMOKE_not_set"
  echo "Set ALLOW_GPU_SMOKE=1 only when HK Lane A is not using the GPU."
  exit 77
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${OUT:-$ROOT/artifacts/d3d9-metal-headless}"
traces=(
  "$ROOT/traces/runtime_samples/d3d9_fixed_function_triangle_runtime.jsonl"
  "$ROOT/traces/runtime_samples/d3d9_fixed_function_texture_modulate_runtime.jsonl"
  "$ROOT/traces/runtime_samples/d3d9_fixed_function_alpha_test_runtime.jsonl"
  "$ROOT/traces/runtime_samples/d3d9_xna_programmable_sprite_runtime.jsonl"
  "$ROOT/traces/runtime_samples/d3d9_xna_alpha_blend_sprite_runtime.jsonl"
  "$ROOT/traces/runtime_samples/d3d9_format_sweep_runtime.jsonl"
)

rm -rf "$OUT"
mkdir -p "$OUT"

for trace in "${traces[@]}"; do
  name="$(basename "$trace" .jsonl)"
  run_out="$OUT/$name"
  mkdir -p "$run_out"
  python3 "$ROOT/tools/d3d_trace_replay.py" "$trace" \
    --backend metal \
    --output-dir "$run_out" \
    --fail-on-unsupported \
    --json > "$run_out/result.json"
done

python3 - "$OUT" <<'PY'
import json
import sys
from pathlib import Path

root = Path(sys.argv[1])
results = sorted(root.glob("*/result.json"))
if not results:
    raise SystemExit("no D3D9 Metal smoke results")

skips = 0
for result in results:
    payload = json.loads(result.read_text())
    print(f"trace={payload['trace_path']}")
    print(f"  status={payload['status']}")
    print(f"  backend={payload['backend']}")
    print(f"  present_count={payload['present_count']}")
    print(f"  non_background_pixels={payload['non_background_pixels']}")
    print(f"  warnings={payload['warnings']}")
    if payload["status"] == "SKIP":
        skips += 1
        continue
    if payload["status"] != "PASS":
        raise SystemExit(f"{result}: status {payload['status']}")
    if payload["present_count"] < 1:
        raise SystemExit(f"{result}: present_count missing")
    if payload["unsupported_calls"]:
        raise SystemExit(f"{result}: unsupported D3D9 calls present")
    if payload["non_background_pixels"] <= 500:
        raise SystemExit(f"{result}: not enough rendered pixels")

if skips:
    raise SystemExit(f"d3d9_metal_headless_smoke=SKIP skipped={skips}")

print(f"d3d9_metal_trace_count={len(results)}")
print("d3d9_metal_headless_smoke=PASS")
PY
