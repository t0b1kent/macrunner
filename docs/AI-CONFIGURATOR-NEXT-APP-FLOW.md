# AI Configurator — Next App Bootstrap Flow

This document explains how to onboard a new Windows application into the MacRunner AI Configurator pipeline.

## Prerequisites

- A working MacRunner build with Wine, hyperbridge, and helper tools.
- App installer or portable executable for the target app.
- A Wine prefix with the app installed (or willingness to install).

## Steps

### 1. Copy the template app profile

```bash
cp profiles/apps/template-app.json profiles/apps/<your-app>.json
```

Edit:
- `app_name`, `app_version`, `arch`
- `exe_path` (inside the Wine prefix)
- `launch_args` (if any)
- `working_directory`

### 2. Set prefix and install the app

Install the app into a fresh or existing Wine prefix. Note the exact path to the `.exe`.

### 3. Run basic PE / app inspection

```bash
# Verify the executable is the expected architecture
x86_64-w64-mingw32-objdump -p "prefix/drive_c/Program Files/App/app.exe" | grep "Dll Name"
```

If unexpected CRT or architecture mismatches appear, note them in `known_yellow_checks`.

### 4. Collect artifacts — first run

```bash
# Run the app via the orchestrator or directly
./scripts/run-<app>.sh
```

Capture:
- `stderr.log`
- `ui-smoke.result.txt`
- `exit.code`
- `infra.status`
- Screenshots / CG captures if available

### 5. Run the AI Configurator

```bash
python3 tools/ai_configurator/macrunner_configurator.py \
  --app profiles/apps/<your-app>.json \
  --artifacts reports/phase-h/<latest-run>
```

The configurator will:
- Auto-detect the latest run if `--artifacts` is omitted.
- Run compatibility + visual classifiers.
- Produce a unified result with category, confidence, next probe, and Codex recommendation.

### 6. Generate Codex task

```bash
python3 tools/ai_configurator/generate_codex_task.py
```

Produces:
- `reports/ai-configurator/CODEX-NEXT-TASK.md`

This contains exact instructions for Codex: what to fix, what NOT to fix, how to verify, and acceptance criteria.

### 7. Apply generic fix (Codex or manual)

Use the generated task markdown as input to Codex. Codex will:
- Edit engine/wine/helper/runtime files **only if** `safe_to_touch_runtime = true`.
- Add regression tests where required.
- Rebuild and relink as needed.

### 8. Add regression

Every fix must include a regression test:
- HyperBridge: add to `hb_test_runner.c`
- Wine: add to existing test harness or document in `reports/phase-h/`
- Helper: add to helper test suite or document in `reports/phase-h/`

### 9. Update compatibility database

If a new symptom was discovered, append it to:
- `reports/compat-learning/<app>-lessons.json`
- Regenerate `NOTEPADPP-LESSONS.md` (or equivalent)

### 10. Move to next app

Once the current app reaches `PRODUCT_GREEN`, move to the next app in the pipeline.

## Safety Rules (Always)

- Never edit engine/wine/helper code if `safe_to_touch_runtime = false`.
- Never fake a PASS classification.
- Always verify with CG ground truth before declaring visual metrics green.
- Always add regression tests for engine fixes.
- Never commit binaries or screenshots in patches.

## See Also

- `profiles/schema/ai-configurator.schema.json` — result schema
- `tools/ai_configurator/macrunner_configurator.py` — orchestrator
- `tools/ai_configurator/generate_codex_task.py` — Codex prompt generator
- `tools/ai_configurator/verify_ai_configurator.sh` — validation script
