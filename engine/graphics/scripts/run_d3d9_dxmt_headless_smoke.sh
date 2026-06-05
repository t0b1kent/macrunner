#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${OUT:-$ROOT/artifacts/d3d9-dxmt-headless}"

if [[ -n "${TRACE:-}" ]]; then
  traces=("$TRACE")
else
  traces=(
    "$ROOT/traces/runtime_samples/d3d9_fixed_function_triangle_runtime.jsonl"
    "$ROOT/traces/runtime_samples/d3d8_renderware_fixed_function_runtime.jsonl"
    "$ROOT/traces/runtime_samples/d3d9_fixed_function_transform_runtime.jsonl"
    "$ROOT/traces/runtime_samples/d3d9_fixed_function_texture_modulate_runtime.jsonl"
    "$ROOT/traces/runtime_samples/d3d9_fixed_function_texture_blend_runtime.jsonl"
    "$ROOT/traces/runtime_samples/d3d9_fixed_function_alpha_test_runtime.jsonl"
    "$ROOT/traces/runtime_samples/d3d9_depth_test_runtime.jsonl"
    "$ROOT/traces/runtime_samples/d3d9_indexed_range_runtime.jsonl"
    "$ROOT/traces/runtime_samples/d3d9_index32_triangle_runtime.jsonl"
    "$ROOT/traces/runtime_samples/d3d9_sampler_linear_wrap_runtime.jsonl"
    "$ROOT/traces/runtime_samples/d3d9_xna_programmable_sprite_runtime.jsonl"
    "$ROOT/traces/runtime_samples/d3d9_xna_alpha_blend_sprite_runtime.jsonl"
    "$ROOT/traces/runtime_samples/d3d9_format_sweep_runtime.jsonl"
  )
fi

rm -rf "$OUT"
mkdir -p "$OUT"

for trace in "${traces[@]}"; do
  name="$(basename "$trace" .jsonl)"
  run_out="$OUT/$name"
  mkdir -p "$run_out"
  json="$(python3 "$ROOT/tools/d3d_trace_replay.py" "$trace" \
    --backend mock \
    --output-dir "$run_out" \
    --fail-on-unsupported \
    --json)"
  printf '%s\n' "$json" > "$run_out/result.json"
done

python3 - "$OUT" <<'PY'
import json
import sys
from pathlib import Path

root = Path(sys.argv[1])
results = sorted(root.glob("*/result.json"))
if not results:
    raise SystemExit("no D3D9 smoke results")

for result in results:
    payload = json.loads(result.read_text())
    trace_name = Path(payload["trace_path"]).name
    min_pixels = {
        "d3d9_fixed_function_alpha_test_runtime.jsonl": 500,
        "d3d9_format_sweep_runtime.jsonl": 900,
        "d3d9_fixed_function_transform_runtime.jsonl": 900,
        "d3d9_indexed_range_runtime.jsonl": 350,
    }.get(trace_name, 1000)
    print(f"trace={payload['trace_path']}")
    print(f"  status={payload['status']}")
    print(f"  api={payload['api']}")
    print(f"  backend={payload['backend']}")
    print(f"  present_count={payload['present_count']}")
    print(f"  non_background_pixels={payload['non_background_pixels']}")
    print(f"  unsupported_calls={payload['unsupported_calls']}")
    print(f"  ppm_path={payload['ppm_path']}")
    if payload["status"] != "PASS":
        raise SystemExit(f"{result}: status {payload['status']}")
    if payload["present_count"] < 1:
        raise SystemExit(f"{result}: present_count missing")
    if payload["unsupported_calls"]:
        raise SystemExit(f"{result}: unsupported D3D9 calls present")
    if payload["non_background_pixels"] <= min_pixels:
        raise SystemExit(f"{result}: not enough rendered pixels")

print(f"d3d9_trace_count={len(results)}")
print("d3d9_dxmt_headless_smoke=PASS")
PY
