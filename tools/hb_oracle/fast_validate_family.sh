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
  rep_movs)
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
    )
    ;;
  string_ops)
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
      "$ROOT/tools/hb_filecheck/examples/string_ops_cmps_lods.check"
    )
    ;;
  *)
    echo "unknown family: $FAMILY"
    exit 2
    ;;
esac
hb_runner_status="not_available"
hb_trace="$TMP_DIR/hb_test_runner_${FAMILY}.trace.txt"
fixture_total=1
if (cd "$ROOT/engine/hyperbridge" && make tests/hb_test_runner >/dev/null && ./tests/hb_test_runner --fast-family "$FAMILY" > "$hb_trace" 2>&1); then
  fixture_pass=1
  hb_runner_status="pass"
else
  status="FAIL"
  hb_runner_status="fail"
  notes+=("hb_test_runner fast-family fail: $FAMILY")
fi

for c in "${checks[@]}"; do
  check_total=$((check_total + 1))
  base="$(basename "$c")"
  if python3 "$ROOT/tools/hb_filecheck/hb_filecheck.py" --input "$hb_trace" --check "$c" >/dev/null; then
    check_pass=$((check_pass + 1))
  else
    status="FAIL"
    notes+=("filecheck fail: $base")
  fi
done

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
