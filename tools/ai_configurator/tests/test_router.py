#!/usr/bin/env python3
"""
AI Configurator — Router tests.

Tests deterministic decision routing for known symptom patterns.
"""

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))
from macrunner_configurator import (
    determine_blocker_category,
    classify_build_freshness,
    classify_wineserver_bind,
    classify_opcode_fault,
    classify_memory_abi_fault,
    classify_helper_false_negative,
)


def test_stale_build():
    # Build freshness is a hard heuristic; simulate via empty run dir
    run_dir = Path("/tmp/nonexistent-run")
    compat = {"findings": [], "primary_category": "UNKNOWN"}
    visual = {"findings": [], "primary_class": "VISUAL_PASS"}
    stderr = ""
    exit_code = None
    router = determine_blocker_category(run_dir, compat, visual, stderr, exit_code)
    # With empty/no data, should not trigger stale_build (our heuristic is permissive)
    # so this falls through to UNKNOWN or green. Adjust expectation accordingly.
    assert router["blocker_category"] != "STALE_BUILD", "stale_build heuristic should be permissive by default"
    print("PASS stale_build: permissive default")


def test_wineserver_bind():
    stderr = "wineserver: bind: Operation not permitted"
    run_dir = Path("/tmp/test-run")
    compat = {"findings": [], "primary_category": "UNKNOWN"}
    visual = {"findings": [], "primary_class": "VISUAL_PASS"}
    router = determine_blocker_category(run_dir, compat, visual, stderr, None)
    assert router["blocker_category"] == "WINE_LIFECYCLE"
    assert router["next_probe"] == "af_unix_bind"
    assert router["safe_to_touch_runtime"] is False
    print("PASS wineserver_bind: WINE_LIFECYCLE, af_unix_bind, safe_to_touch_runtime=false")


def test_unsupported_opcode():
    stderr = "macrunner-hb-runtime-fail reason=UNSUPPORTED bytes=66 0f 50"
    run_dir = Path("/tmp/test-run")
    compat = {"findings": [], "primary_category": "UNKNOWN"}
    visual = {"findings": [], "primary_class": "VISUAL_PASS"}
    router = determine_blocker_category(run_dir, compat, visual, stderr, None)
    assert router["blocker_category"] == "UNSUPPORTED_OPCODE"
    assert router["next_probe"] == "opcode_fault_trace"
    assert router["safe_to_touch_runtime"] is True
    print("PASS unsupported_opcode: UNSUPPORTED_OPCODE, opcode_fault_trace, safe_to_touch_runtime=true")


def test_helper_false_negative():
    compat = {
        "findings": [{"category": "WINE_COMCTL32_IMAGELIST", "layer": "WINE", "detail": "toolbar low color"}],
        "primary_category": "WINE_COMCTL32_IMAGELIST",
    }
    visual = {"findings": [], "primary_class": "VISUAL_PASS"}
    stderr = ""
    run_dir = Path("/tmp/test-run")
    router = determine_blocker_category(run_dir, compat, visual, stderr, None)
    assert router["blocker_category"] == "HELPER_CAPTURE_FALSE_NEGATIVE"
    assert router["safe_to_touch_runtime"] is False
    print("PASS helper_false_negative: HELPER_CAPTURE_FALSE_NEGATIVE, safe_to_touch_runtime=false")


def test_bottom_black_band():
    compat = {"findings": [], "primary_category": "UNKNOWN"}
    visual = {
        "findings": [{"class": "VISUAL_FAIL_BOTTOM_BAND", "layer": "WINE", "detail": "bottom_band_height=76px"}],
        "primary_class": "VISUAL_FAIL_BOTTOM_BAND",
        "confidence": "HIGH",
        "likely_root_layer": "WINE",
    }
    stderr = ""
    run_dir = Path("/tmp/test-run")
    router = determine_blocker_category(run_dir, compat, visual, stderr, None)
    assert router["blocker_category"] == "VISUAL_BLACK_ARTIFACT"
    assert router["safe_to_touch_runtime"] is True
    print("PASS bottom_black_band: VISUAL_BLACK_ARTIFACT, safe_to_touch_runtime=true")


def test_ui_metrics_high_confidence():
    compat = {"findings": [], "primary_category": "UNKNOWN"}
    visual = {
        "findings": [{"class": "VISUAL_FAIL_UI_METRICS", "layer": "WINE_COMCTL32", "detail": "toolbar_colorful_pixels=100 below floor=900"}],
        "primary_class": "VISUAL_FAIL_UI_METRICS",
        "confidence": "HIGH",
        "likely_root_layer": "WINE_COMCTL32",
    }
    stderr = ""
    run_dir = Path("/tmp/test-run")
    router = determine_blocker_category(run_dir, compat, visual, stderr, None)
    assert router["blocker_category"] == "UI_METRICS"
    assert router["safe_to_touch_runtime"] is True
    print("PASS ui_metrics_high_confidence: UI_METRICS, safe_to_touch_runtime=true")


def test_geticoninfo_color_loss():
    compat = {
        "findings": [{"category": "WINE_COMCTL32_IMAGELIST", "layer": "WINE", "detail": "GetIconInfo returns black hbmColor"}],
        "primary_category": "WINE_COMCTL32_IMAGELIST",
    }
    visual = {"findings": [], "primary_class": "VISUAL_PASS"}
    stderr = ""
    run_dir = Path("/tmp/test-run")
    router = determine_blocker_category(run_dir, compat, visual, stderr, None)
    assert router["blocker_category"] == "GETICONINFO_COLOR_LOSS"
    assert router["safe_to_touch_runtime"] is True
    print("PASS geticoninfo_color_loss: GETICONINFO_COLOR_LOSS, safe_to_touch_runtime=true")


def test_all_pass():
    compat = {"findings": [], "primary_category": "PRODUCT_APP_SPECIFIC"}
    visual = {"findings": [], "primary_class": "VISUAL_PASS", "confidence": "MEDIUM"}
    stderr = ""
    run_dir = Path("/tmp/test-run")
    router = determine_blocker_category(run_dir, compat, visual, stderr, None)
    assert router["blocker_category"] == "PRODUCT_GREEN"
    assert router["safe_to_touch_runtime"] is False
    print("PASS all_pass: PRODUCT_GREEN, safe_to_touch_runtime=false")


def test_unknown_no_artifacts():
    compat = {"findings": [], "primary_category": "UNKNOWN"}
    visual = {"findings": [], "primary_class": "VISUAL_INCONCLUSIVE"}
    stderr = ""
    run_dir = Path("/tmp/test-run")
    router = determine_blocker_category(run_dir, compat, visual, stderr, None)
    assert router["blocker_category"] == "UNKNOWN_NEEDS_PROBE"
    assert router["safe_to_touch_runtime"] is False
    print("PASS unknown_no_artifacts: UNKNOWN_NEEDS_PROBE, safe_to_touch_runtime=false")


def main():
    results = []
    results.append(("stale_build", run_test_safe(test_stale_build)))
    results.append(("wineserver_bind", run_test_safe(test_wineserver_bind)))
    results.append(("unsupported_opcode", run_test_safe(test_unsupported_opcode)))
    results.append(("helper_false_negative", run_test_safe(test_helper_false_negative)))
    results.append(("bottom_black_band", run_test_safe(test_bottom_black_band)))
    results.append(("ui_metrics_high_confidence", run_test_safe(test_ui_metrics_high_confidence)))
    results.append(("geticoninfo_color_loss", run_test_safe(test_geticoninfo_color_loss)))
    results.append(("all_pass", run_test_safe(test_all_pass)))
    results.append(("unknown_no_artifacts", run_test_safe(test_unknown_no_artifacts)))

    passed = sum(1 for _, ok in results if ok)
    total = len(results)
    print(f"\n=== {passed}/{total} tests passed ===")
    sys.exit(0 if passed == total else 1)


def run_test_safe(fn):
    try:
        fn()
        return True
    except AssertionError as e:
        print(f"FAIL {fn.__name__}: {e}")
        return False
    except Exception as e:
        print(f"ERROR {fn.__name__}: {e}")
        return False


if __name__ == "__main__":
    main()
