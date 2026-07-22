# HK Oracle Managed Actuator Capability Checkpoint

Classification: `VERIFIED_CAPABILITY_NOT_GOLDEN`

Machine verdict: `SCENE_CAPABILITY_PASS_RENDER_BLACK`

Source run:

`reports/phase4-hollow-knight/laneA-oracle-managed-actuator-20260722-try1-105203`

Report:

`reports/phase4-hollow-knight/PIXEL-FIRST-ORACLE-MANAGED-ACTUATOR-RESULT.md`

This compact checkpoint anchors the no-allocation managed actuator run where `SetLanguage("EN") -> ConfirmLanguage()` returned OK, `GameManager`/`UIManager`/`GameCameras` were created, `Present/Present1` reached `48/48`, and every required capture remained `BLACK` with `non_black_px=0`.

It intentionally excludes full `run.log`, raw `final-child.json`, and translation-cache bytes.
