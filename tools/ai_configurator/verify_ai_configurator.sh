#!/usr/bin/env bash
set -eu

ROOT="/Volumes/MacOS/MacRunner"
REPORTS="$ROOT/reports/ai-configurator"
TOOLS="$ROOT/tools/ai_configurator"
PROFILES="$ROOT/profiles"

echo "========================================"
echo "AI Configurator MVP v0.1 Verification"
echo "========================================"
echo ""

# 1. py_compile
PASS=0
FAIL=0

echo "--- 1. Python syntax validation ---"
for py in "$TOOLS/macrunner_configurator.py" "$TOOLS/generate_codex_task.py" "$TOOLS/tests/test_router.py"; do
    if python3 -m py_compile "$py"; then
        echo "PASS $(basename "$py")"
        ((PASS+=1)) || true
    else
        echo "FAIL $(basename "$py")"
        ((FAIL+=1)) || true
    fi
done
echo ""

# 2. Router tests
echo "--- 2. Router tests ---"
if python3 "$TOOLS/tests/test_router.py"; then
    echo "PASS router tests"
    ((PASS+=1)) || true
else
    echo "FAIL router tests"
    ((FAIL+=1)) || true
fi
echo ""

# 3. JSON validation
echo "--- 3. JSON validation ---"
for json in "$PROFILES/schema/ai-configurator.schema.json" "$PROFILES/apps/template-app.json" "$REPORTS/ai-configurator-dashboard.json"; do
    if python3 -c "import json; json.load(open('$json'))"; then
        echo "PASS $(basename "$json")"
        ((PASS+=1)) || true
    else
        echo "FAIL $(basename "$json")"
        ((FAIL+=1)) || true
    fi
done
echo ""

# 4. Run configurator on latest Notepad++ artifacts if available
echo "--- 4. Configurator run on latest Notepad++ artifacts ---"
LATEST_RUN=$(find "$ROOT/reports/phase-h" -maxdepth 1 -name "npp-x64-*" -type d | sort -t- -k5,5 -k6,6 | tail -1 || true)
if [ -n "$LATEST_RUN" ] && [ -d "$LATEST_RUN" ]; then
    echo "Found run: $LATEST_RUN"
    if python3 "$TOOLS/macrunner_configurator.py" \
        --app "$PROFILES/apps/notepadpp.json" \
        --artifacts "$LATEST_RUN" \
        --out-dir "$REPORTS"; then
        echo "PASS configurator run"
        ((PASS+=1)) || true
    else
        echo "FAIL configurator run"
        ((FAIL+=1)) || true
    fi
else
    echo "No Notepad++ run found. Skipping configurator run."
fi
echo ""

# 5. Generate Codex next task if configurator result exists
echo "--- 5. Generate Codex next task ---"
if [ -f "$REPORTS/latest-configurator-result.json" ]; then
    if python3 "$TOOLS/generate_codex_task.py" \
        --result "$REPORTS/latest-configurator-result.json" \
        --out "$REPORTS/CODEX-NEXT-TASK.md"; then
        echo "PASS Codex task generation"
        ((PASS+=1)) || true
    else
        echo "FAIL Codex task generation"
        ((FAIL+=1)) || true
    fi
else
    echo "No configurator result. Skipping Codex task generation."
fi
echo ""

echo "========================================"
echo "Verification complete."
echo "========================================"
echo "PASS: $PASS"
echo "FAIL: $FAIL"
echo ""
echo "Reports: $REPORTS/"
echo "Tools:   $TOOLS/"
echo ""
echo "Must NOT have launched Wine, built Wine, or edited engine."
echo "If any of those happened, this script is broken."
