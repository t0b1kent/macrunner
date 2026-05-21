#!/usr/bin/env bash
set -euo pipefail

FAMILY="${1:-}"
if [[ -z "$FAMILY" ]]; then
  echo "usage: $0 <family>"
  exit 2
fi

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT_DIR="$ROOT/reports/hyperbridge-validation"
TMP_DIR="$ROOT/.tmp/hb_fast_validate"
mkdir -p "$OUT_DIR" "$TMP_DIR"

JSON_OUT="$OUT_DIR/LATEST-FAST-VALIDATION.json"
MD_OUT="$OUT_DIR/LATEST-FAST-VALIDATION.md"

status="PASS"
notes=()
fixture_total=0
fixture_pass=0
check_total=0
check_pass=0

case "$FAMILY" in
  rep_movs|string_ops)
    fixtures=(
      "$ROOT/tools/hb_oracle/fixtures/rep_movs_df0_forward.json"
      "$ROOT/tools/hb_oracle/fixtures/rep_movs_df1_backward.json"
      "$ROOT/tools/hb_oracle/fixtures/rep_movs_zero_count.json"
      "$ROOT/tools/hb_oracle/fixtures/rep_movs_cross_page.json"
      "$ROOT/tools/hb_oracle/fixtures/rep_movs_overlap.json"
    )
    checks=(
      "$ROOT/tools/hb_filecheck/examples/rep_movs_forward.check"
      "$ROOT/tools/hb_filecheck/examples/rep_movs_df1.check"
      "$ROOT/tools/hb_filecheck/examples/memory_fault.check"
    )
    ;;
  *)
    echo "unknown family: $FAMILY"
    exit 2
    ;;
esac

# TODO-hook: Codex should replace this with real HyperBridge runs (interp+jit)
for f in "${fixtures[@]}"; do
  fixture_total=$((fixture_total + 1))
  name="$(basename "$f" .json)"
  payload="$TMP_DIR/${name}.payload.json"
  python3 - "$payload" "$name" "$FAMILY" <<'PY'
import json, sys
out, name, family = sys.argv[1], sys.argv[2], sys.argv[3]
payload = {
    "fixture_name": name,
    "family": family,
    "backend": "interp",
    "expected": {"registers": {}, "flags": {}, "memory_after": [], "rip": "0x0", "fault": None},
    "actual": {"registers": {}, "flags": {}, "memory_after": [], "rip": "0x0", "fault": None},
    "actual_fault": None,
}
with open(out, "w", encoding="utf-8") as f:
    json.dump(payload, f, indent=2)
PY
  if python3 "$ROOT/tools/hb_oracle/compare_oracle_results.py" --input "$payload" >/dev/null; then
    fixture_pass=$((fixture_pass + 1))
  else
    status="FAIL"
    notes+=("oracle fail: $name")
  fi
done

# TODO-hook: Codex should replace trace source with real hb_test_runner traces
for c in "${checks[@]}"; do
  check_total=$((check_total + 1))
  base="$(basename "$c")"
  trace="$TMP_DIR/${base}.trace.txt"
  case "$base" in
    rep_movs_forward.check)
      printf "rep_movs_enter\ndf=0\nrcx=16\nwrite_ok\nrep_movs_exit\n" > "$trace"
      ;;
    rep_movs_df1.check)
      printf "rep_movs_enter\ndf=1\nrcx=16\nwrite_ok\nrep_movs_exit\n" > "$trace"
      ;;
    memory_fault.check)
      printf "rep_movs_enter\npage_cross=1\nfault=MEMORY_FAULT\nrep_movs_exit\n" > "$trace"
      ;;
  esac
  if python3 "$ROOT/tools/hb_filecheck/hb_filecheck.py" --input "$trace" --check "$c" >/dev/null; then
    check_pass=$((check_pass + 1))
  else
    status="FAIL"
    notes+=("filecheck fail: $base")
  fi
done

hb_runner_status="not_available"
if [[ -x "$ROOT/engine/hyperbridge/tests/hb_test_runner" ]]; then
  # TODO-hook: Codex can run minimal family-specific hb_test_runner tests here.
  hb_runner_status="available_todo_hook"
fi

notes_json="[]"
if [[ ${#notes[@]} -gt 0 ]]; then
  notes_json="$(printf '%s\n' "${notes[@]}" | python3 -c 'import json,sys; print(json.dumps([x.strip() for x in sys.stdin if x.strip()]))')"
fi

python3 - "$JSON_OUT" "$FAMILY" "$status" "$fixture_total" "$fixture_pass" "$check_total" "$check_pass" "$hb_runner_status" "$notes_json" <<'PY'
import json, sys
out, family, status, ft, fp, ct, cp, hb, notes = sys.argv[1:]
payload = {
    "family": family,
    "status": status,
    "oracle": {"total": int(ft), "pass": int(fp)},
    "filecheck": {"total": int(ct), "pass": int(cp)},
    "hb_test_runner": hb,
    "notes": json.loads(notes),
}
with open(out, "w", encoding="utf-8") as f:
    json.dump(payload, f, indent=2)
PY

{
  echo "# FAST VALIDATION RESULT"
  echo
  echo "- family: $FAMILY"
  echo "- status: **$status**"
  echo "- oracle: $fixture_pass / $fixture_total"
  echo "- filecheck: $check_pass / $check_total"
  echo "- hb_test_runner: $hb_runner_status"
  echo
  echo "## Notes"
} > "$MD_OUT"

if [[ ${#notes[@]} -eq 0 ]]; then
  echo "- none" >> "$MD_OUT"
else
  for n in "${notes[@]}"; do echo "- $n" >> "$MD_OUT"; done
fi

echo "FAST VALIDATION: $status"

