#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "tools/hk_language_input.py").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


require('KEY_CODES = {"return": 36, "right": 124}' in SOURCE, "sealed key codes drift")
require('expected = [] if plan.get("mode") == "A" else ["return", "right", "return"]' in SOURCE,
        "A/B event sequence drift")
require("CGEventPostToPid" in SOURCE, "events must target the proven HK window PID")
require('window.get("owner") == "wine"' in SOURCE and 'window.get("name") == "Hollow Knight"' in SOURCE,
        "exact target window predicate missing")
require("SetLanguage acceptance" in SOURCE, "first Submit semantic proof missing")
require("LanguageConfirm CancelButton preselection" in SOURCE, "confirm panel readiness proof missing")
require("ConfirmLanguage-after-horizontal" in SOURCE, "Horizontal acceptance proof missing")
require("ConfirmLanguage-call-1" in SOURCE, "final Submit acceptance proof missing")
require("logical_events=3, cg_events=6" in SOURCE, "logical/low-level event accounting missing")
for forbidden in ("mono_runtime_invoke", "ConfirmLanguage()", "GameLangSet", "PointerClick"):
    require(forbidden not in SOURCE, f"input harness must not bypass first-run UI: {forbidden}")

print("PASS: HK sealed three-event targeted input source contract")
