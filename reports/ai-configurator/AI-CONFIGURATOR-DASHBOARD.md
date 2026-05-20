# AI Configurator Dashboard

## Current App

- **App:** Notepad++
- **Profile:** `profiles/apps/notepadpp.json`
- **Arch:** x64
- **Version:** 8.7.5

## Current Status

- **Blocker category:** see latest-configurator-result.json
- **Confidence:** see latest-configurator-result.json
- **Safe to touch runtime:** see latest-configurator-result.json
- **Stage:** see latest-configurator-result.json

## Recommended Codex Task

See `CODEX-NEXT-TASK.md` for the exact next task.

## Last Classifications

- **Compatibility:** `reports/compat-learning/LATEST-CLASSIFICATION.md`
- **Visual:** `reports/visual-regression/LATEST-VISUAL-CLASSIFICATION.md`

## Next Probe

See `latest-configurator-result.json` → `next_probe`.

## Known Limitations

- Helper capture readback may diverge from CG ground truth.
- Font/UI metrics require Windows baseline artifact before green.
- Menu cross-process enumeration not reliable under Wine (resource fallback used).
- Clean exit timeout alignment pending.

## Next App Readiness

1. Copy `profiles/apps/template-app.json` → `profiles/apps/<your-app>.json`
2. Fill `exe_path`, `launch_args`, `working_directory`
3. Run `python3 tools/ai_configurator/macrunner_configurator.py --app profiles/apps/<your-app>.json`
4. Review `reports/ai-configurator/LATEST-CONFIGURATOR-RESULT.md`
5. Run `python3 tools/ai_configurator/generate_codex_task.py`
6. Give `CODEX-NEXT-TASK.md` to Codex

## Files

- Orchestrator: `tools/ai_configurator/macrunner_configurator.py`
- Codex generator: `tools/ai_configurator/generate_codex_task.py`
- Verify script: `tools/ai_configurator/verify_ai_configurator.sh`
- Schema: `profiles/schema/ai-configurator.schema.json`
- Template app: `profiles/apps/template-app.json`
- Next-app flow: `docs/AI-CONFIGURATOR-NEXT-APP-FLOW.md`
