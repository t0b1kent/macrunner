#!/usr/bin/env bash
set -eu

ROOT="/Volumes/MacOS/MacRunner"
REPORTS="$ROOT/reports/visual-regression"
TOOLS="$ROOT/tools/visual_regression"
PROFILES="$ROOT/profiles/visual"

echo "========================================"
echo "Visual Regression Lab v1 Verification"
echo "========================================"
echo ""

# 1. JSON validation
echo "--- 1. JSON validation ---"
python3 -c "
import json
files = [
    '$REPORTS/visual-artifact-taxonomy.json',
    '$REPORTS/visual-regression-dashboard.json',
    '$PROFILES/notepadpp-visual-profile.json',
]
ok = 0
for f in files:
    try:
        with open(f) as fh:
            json.load(fh)
        print(f'PASS {f}')
        ok += 1
    except Exception as e:
        print(f'FAIL {f}: {e}')
print(f'=== JSON: {ok}/{len(files)} passed')
"
echo ""

# 2. Tool syntax validation
echo "--- 2. Tool syntax validation ---"
python3 -m py_compile "$TOOLS/analyze_capture.py" && echo "PASS analyze_capture.py" || echo "FAIL analyze_capture.py"
python3 -m py_compile "$TOOLS/classify_visual_state.py" && echo "PASS classify_visual_state.py" || echo "FAIL classify_visual_state.py"
python3 -m py_compile "$TOOLS/generate_visual_fixpack.py" && echo "PASS generate_visual_fixpack.py" || echo "FAIL generate_visual_fixpack.py"
python3 -m py_compile "$TOOLS/collect_latest_artifacts.py" && echo "PASS collect_latest_artifacts.py" || echo "FAIL collect_latest_artifacts.py"
python3 -m py_compile "$TOOLS/tests/test_visual_classifier.py" && echo "PASS test_visual_classifier.py" || echo "FAIL test_visual_classifier.py"
echo ""

# 3. Visual classifier tests
echo "--- 3. Visual classifier tests ---"
python3 "$TOOLS/tests/test_visual_classifier.py"
echo ""

# 4. Collect latest artifacts (read-only)
echo "--- 4. Collect latest artifacts ---"
python3 "$TOOLS/collect_latest_artifacts.py"
echo ""

# 5. If a capture exists, analyze it
echo "--- 5. Analyze latest capture if available ---"
LATEST_BMP=$(find "$ROOT/reports/phase-h" -name "toolbar-cg-capture.bmp" -maxdepth 2 2>/dev/null | sort -t- -k5,5 -k6,6 | tail -1 || true)
if [ -n "$LATEST_BMP" ] && [ -f "$LATEST_BMP" ]; then
    echo "Found capture: $LATEST_BMP"
    python3 "$TOOLS/analyze_capture.py" "$LATEST_BMP" --out-dir "$REPORTS" || echo "WARN: analyze_capture.py failed"
else
    echo "No capture found. Skipping analysis."
fi
echo ""

# 6. Generate classification (requires analysis JSON)
echo "--- 6. Generate visual classification ---"
if [ -f "$REPORTS/latest-visual-analysis.json" ]; then
    python3 "$TOOLS/classify_visual_state.py" \
        --analysis "$REPORTS/latest-visual-analysis.json" \
        --profile "$PROFILES/notepadpp-visual-profile.json" \
        --out-dir "$REPORTS" || echo "WARN: classify_visual_state.py failed"
else
    echo "No analysis JSON. Skipping classification."
fi
echo ""

# 7. Generate Codex fixpack
echo "--- 7. Generate Codex visual fixpack ---"
if [ -f "$REPORTS/latest-visual-classification.json" ]; then
    python3 "$TOOLS/generate_visual_fixpack.py" \
        --classification "$REPORTS/latest-visual-classification.json" \
        --profile "$PROFILES/notepadpp-visual-profile.json" \
        --taxonomy "$REPORTS/visual-artifact-taxonomy.json" \
        --out "$REPORTS/CODEX-VISUAL-NEXT-FIXPACK.md" || echo "WARN: generate_visual_fixpack.py failed"
else
    echo "No classification JSON. Skipping fixpack."
fi
echo ""

echo "========================================"
echo "Verification complete."
echo "========================================"
echo "Reports: $REPORTS/"
echo "Tools:   $TOOLS/"
echo ""
echo "Must NOT have launched Wine, built Wine, or edited engine."
echo "If any of those happened, this script is broken."
