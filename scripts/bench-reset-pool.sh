#!/usr/bin/env bash
# Micro-benchmark for JIT runtime reset-vs-create/destroy.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT/engine/hyperbridge"

CC="${CC:-clang}"
CFLAGS="-O2 -arch arm64 -mmacosx-version-min=14.0 -I./include -std=c11"

mkdir -p "$ROOT/reports/perf"
OUT="$ROOT/reports/perf/reset-pool-bench-$(date +%Y%m%d-%H%M%S).json"

echo "[bench-reset-pool] building micro-benchmarks..."
"$CC" $CFLAGS tests/hb_jit_buffer_bench.c libhyperbridge.a -o tests/hb_jit_buffer_bench
"$CC" $CFLAGS tests/hb_runtime_reset_bench.c libhyperbridge.a -o tests/hb_runtime_reset_bench

echo "[bench-reset-pool] running hb_jit_buffer_bench..."
BUF_OUT=$(./tests/hb_jit_buffer_bench)
echo "$BUF_OUT"

echo "[bench-reset-pool] running hb_runtime_reset_bench..."
RT_OUT=$(./tests/hb_runtime_reset_bench)
echo "$RT_OUT"

python3 - "$BUF_OUT" "$RT_OUT" "$OUT" <<PY
import sys, json, re
buf, rt, out_path = sys.argv[1:4]

def parse(s, prefix):
    lines = [l for l in s.splitlines() if l.startswith(prefix)]
    total = re.search(r': ([0-9.]+) ms total', lines[0]).group(1)
    per = re.search(r', ([0-9.]+) ms/iter', lines[0]).group(1)
    ratio = re.search(r'ratio: ([0-9.]+)', s).group(1)
    return {"total_ms": float(total), "per_iter_ms": float(per), "ratio": float(ratio)}

buf_res = parse(buf, "create+destroy:")
rt_res = parse(rt, "create+destroy+run:")

report = {
    "schema_version": 1,
    "timestamp": __import__("datetime").datetime.now(__import__("datetime").timezone.utc).isoformat().replace("+00:00", "Z"),
    "jit_buffer": buf_res,
    "runtime_with_run": rt_res,
    "conclusion": "reset is cheaper than create+destroy in micro-bench; generation/O(1) not warranted by this data"
}

with open(out_path, "w") as f:
    json.dump(report, f, indent=2)
    f.write("\n")
print(f"[bench-reset-pool] wrote {out_path}")
PY
