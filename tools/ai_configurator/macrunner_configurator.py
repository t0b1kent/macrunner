#!/usr/bin/env python3
"""
MacRunner AI Configurator MVP v0.1 — Unified Orchestrator

Consumes:
  - app profile JSON
  - latest run artifacts (auto-detected if omitted)
  - compat learning classifier output
  - visual regression classifier output
  - probes registry
  - symptom lessons database

Produces:
  - reports/ai-configurator/latest-configurator-result.json
  - reports/ai-configurator/LATEST-CONFIGURATOR-RESULT.md
"""

import argparse
import json
import os
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Optional

sys.path.insert(0, str(Path(__file__).parent.parent / "compat_learning"))
sys.path.insert(0, str(Path(__file__).parent.parent / "visual_regression"))

from classify_app_run import run_classification as compat_classify
from classify_visual_state import classify as visual_classify


ROOT = Path("/Volumes/MacOS/MacRunner")
REPORTS = ROOT / "reports"
PHASE_H = REPORTS / "phase-h"
COMPAT_DIR = REPORTS / "compat-learning"
VISUAL_DIR = REPORTS / "visual-regression"
AI_DIR = REPORTS / "ai-configurator"


def load_json(path: Path) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def find_latest_run(pattern: str = "npp-x64-*") -> Optional[Path]:
    matches = sorted(PHASE_H.glob(pattern), key=lambda p: p.stat().st_mtime, reverse=True)
    return matches[0] if matches else None


def find_file(run_dir: Path, pattern: str) -> Optional[Path]:
    if not run_dir.exists():
        return None
    matches = list(run_dir.glob(pattern))
    return matches[0] if matches else None


def read_ui_smoke_result(run_dir: Path) -> str:
    path = find_file(run_dir, "ui-smoke.result.txt")
    if path and path.exists():
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    return ""


def read_stderr_log(run_dir: Path) -> str:
    path = find_file(run_dir, "stderr.log")
    if path and path.exists():
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    return ""


def read_exit_code(run_dir: Path) -> Optional[int]:
    path = find_file(run_dir, "exit.code")
    if path and path.exists():
        try:
            with open(path, "r") as f:
                return int(f.read().strip())
        except ValueError:
            pass
    return None


def classify_build_freshness(run_dir: Path) -> tuple[bool, Optional[str]]:
    """Heuristic: if no ntdll.so timestamp info available, assume fresh."""
    return False, None


def classify_wineserver_bind(stderr_text: str) -> tuple[bool, Optional[str]]:
    if "wineserver: bind" in stderr_text.lower() or "operation not permitted" in stderr_text.lower():
        return True, "wineserver: bind: Operation not permitted"
    if "af_unix" in stderr_text.lower() and "bind" in stderr_text.lower():
        return True, "AF_UNIX bind failure detected"
    return False, None


def classify_opcode_fault(stderr_text: str) -> tuple[bool, Optional[str]]:
    if "unsupported opcode" in stderr_text.lower() or "macrunner-hb-runtime-fail" in stderr_text.lower():
        # Extract bytes if possible
        import re
        m = re.search(r"bytes=([0-9a-f ]+)", stderr_text, re.IGNORECASE)
        detail = f"unsupported opcode: bytes={m.group(1)}" if m else "unsupported opcode detected"
        return True, detail
    return False, None


def classify_memory_abi_fault(stderr_text: str) -> tuple[bool, Optional[str]]:
    if "c0000005" in stderr_text.lower():
        return True, "memory fault c0000005"
    if "c000007b" in stderr_text.lower():
        return True, "wrong-arch callback c000007b"
    if "bad return" in stderr_text.lower():
        return True, "bad return path"
    return False, None


def classify_helper_false_negative(compat_result: dict, visual_result: dict) -> tuple[bool, Optional[str]]:
    # If compat says toolbar issue and visual says PASS, it's a helper false-negative.
    # Exclude GetIconInfo color loss — that is a real runtime defect even if visual passes.
    compat_findings = compat_result.get("findings", [])
    for f in compat_findings:
        cat = f.get("category", "").lower()
        detail = f.get("detail", "").lower()
        if "geticoninfo" in detail:
            continue
        if "helper_capture_false_negative" in cat and visual_result.get("primary_class") == "VISUAL_PASS":
            return True, "compat indicates helper capture false-negative"
        if ("toolbar" in cat or "toolbar" in detail) and visual_result.get("primary_class") == "VISUAL_PASS":
            return True, "compat indicates toolbar issue but visual analysis passes"
        if "imagelist" in cat and ("capture" in detail or "readback" in detail) and visual_result.get("primary_class") == "VISUAL_PASS":
            return True, "compat indicates ImageList capture/readback issue but visual analysis passes"
    return False, None


def determine_blocker_category(
    run_dir: Path,
    compat_result: dict,
    visual_result: dict,
    stderr_text: str,
    exit_code: Optional[int],
) -> dict:
    """
    Deterministic decision router.
    Returns dict with blocker_category, confidence, next_probe, safe_to_touch_runtime, etc.
    """

    # Priority 1: Infra / stale build
    stale, stale_detail = classify_build_freshness(run_dir)
    if stale:
        return {
            "blocker_category": "STALE_BUILD",
            "current_blocker": stale_detail or "Stale build artifacts detected",
            "confidence": "HIGH",
            "next_probe": "build_freshness",
            "safe_to_touch_runtime": False,
            "recommended_codex_action": "BUILD_FRESHNESS_FIX: remove stale objects, rebuild, relink, codesign, rerun.",
            "forbidden_actions": ["Do NOT patch engine or Wine before verifying build freshness."],
            "acceptance_criteria": [
                "libhyperbridge.a timestamp newer than source",
                "ntdll.so symbols include hb_decode/hyperbridge",
                "Smoke rerun passes without stale-build warnings"
            ],
            "rollback_rules": ["Revert any manual object deletes if they break the build system."],
        }

    # Priority 2: AF_UNIX / wineserver bind
    bind_fail, bind_detail = classify_wineserver_bind(stderr_text)
    if bind_fail:
        return {
            "blocker_category": "WINE_LIFECYCLE",
            "current_blocker": bind_detail,
            "confidence": "HIGH",
            "next_probe": "af_unix_bind",
            "safe_to_touch_runtime": False,
            "recommended_codex_action": "WINE_LIFECYCLE_FIX: verify AF_UNIX socket permissions, check sandbox, retry from permitted session.",
            "forbidden_actions": ["Do NOT patch app or engine for a wineserver sandbox issue."],
            "acceptance_criteria": [
                "af_unix_bind probe returns PASS",
                "wineserver starts without 'Operation not permitted'",
                "App process appears in ps"
            ],
            "rollback_rules": ["No runtime changes needed; fix is infrastructure/sandbox."],
        }

    # Priority 3: Unsupported opcode
    opcode_fail, opcode_detail = classify_opcode_fault(stderr_text)
    if opcode_fail:
        return {
            "blocker_category": "UNSUPPORTED_OPCODE",
            "current_blocker": opcode_detail,
            "confidence": "HIGH",
            "next_probe": "opcode_fault_trace",
            "safe_to_touch_runtime": True,
            "recommended_codex_action": "OPCODE_FAMILY_FIX: decode exact bytes, implement decoder+IR+lifter+interpreter+JIT, add tests, relink ntdll.",
            "forbidden_actions": [
                "Do NOT implement partial opcode (single variant only).",
                "Do NOT skip regression tests.",
                "Do NOT forget to relink ntdll.so and codesign."
            ],
            "acceptance_criteria": [
                "Decoder recognizes opcode and siblings",
                "IR enum added",
                "Interpreter semantics correct for trigger + siblings",
                "hb_test_runner covers trigger + siblings",
                "Smoke rerun passes past the previous fault PC"
            ],
            "rollback_rules": [
                "Keep old decoder enum values stable to avoid ABI breakage.",
                "If relink fails, revert ntdll.so to backup."
            ],
        }

    # Priority 4: Memory / ABI / return path
    mem_fail, mem_detail = classify_memory_abi_fault(stderr_text)
    if mem_fail:
        cat = "ABI_OR_RETURN_PATH" if "007b" in mem_detail.lower() or "return" in mem_detail.lower() else "MEMORY_FAULT"
        return {
            "blocker_category": cat,
            "current_blocker": mem_detail,
            "confidence": "HIGH",
            "next_probe": "opcode_fault_trace",
            "safe_to_touch_runtime": True,
            "recommended_codex_action": "HYPERBRIDGE_ROOT_FIX: inspect fault PC, IAT/native pointer, stack alignment, callback trampoline.",
            "forbidden_actions": [
                "Do NOT add random bounds checks without identifying root cause.",
                "Do NOT patch Wine DLLs for engine memory faults."
            ],
            "acceptance_criteria": [
                "Fault PC explained by root cause",
                "Regression test reproduces fault before fix",
                "Smoke passes without c0000005/c000007b"
            ],
            "rollback_rules": ["Revert engine changes if smoke becomes worse."],
        }

    # Priority 5: Helper false-negative
    helper_fn, helper_detail = classify_helper_false_negative(compat_result, visual_result)
    if helper_fn:
        return {
            "blocker_category": "HELPER_CAPTURE_FALSE_NEGATIVE",
            "current_blocker": helper_detail,
            "confidence": "MEDIUM",
            "next_probe": "win32_helper_capture",
            "safe_to_touch_runtime": False,
            "recommended_codex_action": "HARNESS_CAPTURE_FIX: fix helper bitmap readback or classification before treating as product defect.",
            "forbidden_actions": [
                "Do NOT touch comctl32/ImageList.",
                "Do NOT instrument Wine DLLs.",
                "Do NOT treat helper capture as product ground truth."
            ],
            "acceptance_criteria": [
                "CG capture and helper capture agree within tolerance",
                "Helper classification matches CG ground truth",
                "Toolbar passes on both CG and helper"
            ],
            "rollback_rules": ["Revert helper changes if they destabilize other probes."],
        }

    # Priority 6: Visual / UI metrics
    visual_primary = visual_result.get("primary_class", "VISUAL_INCONCLUSIVE")
    visual_findings = visual_result.get("findings", [])
    compat_primary = compat_result.get("primary_category", "UNKNOWN")
    compat_findings = compat_result.get("findings", [])

    if visual_primary.startswith("VISUAL_FAIL"):
        # Map visual class to category
        visual_class_map = {
            "VISUAL_FAIL_BOTTOM_BAND": "VISUAL_BLACK_ARTIFACT",
            "VISUAL_FAIL_RIGHT_BAND": "VISUAL_BLACK_ARTIFACT",
            "VISUAL_FAIL_SCROLLBAR_BLACK": "VISUAL_BLACK_ARTIFACT",
            "VISUAL_FAIL_STATUSBAR": "UI_METRICS",
            "VISUAL_FAIL_UI_METRICS": "UI_METRICS",
            "VISUAL_FAIL_FOLDER_ICONS_BLACK": "GETICONINFO_COLOR_LOSS",
        }
        category = visual_class_map.get(visual_primary, "VISUAL_BLACK_ARTIFACT")
        layer = visual_result.get("likely_root_layer", "WINE")
        return {
            "blocker_category": category,
            "current_blocker": f"{visual_primary}: {visual_findings[0]['detail'] if visual_findings else 'visual defect detected'}",
            "confidence": visual_result.get("confidence", "MEDIUM"),
            "next_probe": "ui_metrics",
            "safe_to_touch_runtime": True,
            "recommended_codex_action": "VISUAL_RUNTIME_FIX: inspect rects, traces, DIB/mask paths. Do NOT patch blindly.",
            "forbidden_actions": [
                "Do NOT shrink window size blindly.",
                "Do NOT patch uxtheme/comctl32 without CG proof.",
                "Do NOT mark green without Windows baseline."
            ],
            "acceptance_criteria": [
                "CG capture shows improvement",
                "Black band ratio below threshold",
                "Toolbar colorful pixels above floor"
            ],
            "rollback_rules": ["Revert Wine changes if other apps regress."],
        }

    # Priority 7: GetIconInfo from compat findings
    for f in compat_findings:
        if "geticoninfo" in f.get("detail", "").lower() or f.get("category", "").startswith("WINE_COMCTL32"):
            return {
                "blocker_category": "GETICONINFO_COLOR_LOSS",
                "current_blocker": f"{f['category']}: {f['detail']}",
                "confidence": "MEDIUM",
                "next_probe": "icon_geticoninfo_trace",
                "safe_to_touch_runtime": True,
                "recommended_codex_action": "ICON_PIPELINE_ROOT_FIX: trace HICON -> GetIconInfo -> DrawIconEx -> ImageList -> ListView draw. Check mask inversion.",
                "forbidden_actions": [
                    "Do NOT patch shell32 icon loading.",
                    "Do NOT instrument comctl32/toolbar.c directly."
                ],
                "acceptance_criteria": [
                    "GetIconInfo returns valid hbmColor",
                    "Icon renders correctly in CG capture",
                    "No black mask artifacts in dialog/listview"
                ],
                "rollback_rules": ["Revert if other toolbar apps show color loss."],
            }

    # Priority 8: All pass
    if visual_primary == "VISUAL_PASS" and compat_primary in ("PRODUCT_APP_SPECIFIC", "UNKNOWN", ""):
        # Check if there are still known yellow items
        return {
            "blocker_category": "PRODUCT_GREEN",
            "current_blocker": "No blocker detected by automated analysis.",
            "confidence": "MEDIUM",
            "next_probe": "clean_exit_probe",
            "safe_to_touch_runtime": False,
            "recommended_codex_action": "PERFORMANCE_BASELINE: run clean exit, performance, stress tests. Do NOT touch runtime.",
            "forbidden_actions": [
                "Do NOT patch runtime when product is green.",
                "Do NOT mark metrics green without Windows baseline."
            ],
            "acceptance_criteria": [
                "Clean exit within timeout",
                "No new visual regressions",
                "Performance baseline recorded"
            ],
            "rollback_rules": ["No rollback needed."],
        }

    # Fallback: unknown / needs more probes
    return {
        "blocker_category": "UNKNOWN_NEEDS_PROBE",
        "current_blocker": f"Compat primary={compat_primary}, Visual primary={visual_primary}. Automated routing inconclusive.",
        "confidence": "LOW",
        "next_probe": "win32_helper_capture",
        "safe_to_touch_runtime": False,
        "recommended_codex_action": "RUN_TARGETED_PROBE: collect more evidence (screenshots, traces, stderr) before deciding.",
        "forbidden_actions": [
            "Do NOT patch anything without evidence.",
            "Do NOT fake PASS classification."
        ],
        "acceptance_criteria": [
            "Probe result explains blocker",
            "Classification confidence rises to MEDIUM or HIGH",
            "Codex has exact fix target"
        ],
        "rollback_rules": ["No changes to revert."],
    }


def run_configurator(app_profile_path: Path, artifacts_dir: Optional[Path], mode: str) -> dict:
    profile = load_json(app_profile_path)

    # Auto-detect latest run
    if artifacts_dir is None:
        artifacts_dir = find_latest_run()
        if artifacts_dir is None:
            # No runs available — return unknown
            return {
                "app_profile_path": str(app_profile_path),
                "selected_run_dir": None,
                "current_stage": "unknown",
                "current_blocker": "No phase-h run directories found.",
                "blocker_category": "UNKNOWN_NEEDS_PROBE",
                "confidence": "LOW",
                "next_probe": "win32_helper_capture",
                "safe_to_touch_runtime": False,
                "recommended_codex_action": "RUN_TARGETED_PROBE: no artifacts available. Run smoke first.",
                "generated_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
                "compatibility": {},
                "visual": {},
                "forbidden_actions": ["Do not patch without evidence."],
                "acceptance_criteria": ["Produce at least one run artifact."],
                "rollback_rules": [],
            }

    ui_smoke_text = read_ui_smoke_result(artifacts_dir)
    stderr_text = read_stderr_log(artifacts_dir)
    exit_code = read_exit_code(artifacts_dir)

    # Run classifiers
    try:
        compat_result = compat_classify(artifacts_dir)
    except Exception as e:
        compat_result = {
            "primary_category": "UNKNOWN",
            "findings": [],
            "confidence": "LOW",
            "classifier_error": str(e),
        }

    # Visual classification: try latest-visual-classification.json first, else run classifier
    visual_analysis_path = VISUAL_DIR / "latest-visual-analysis.json"
    visual_result = {}
    if visual_analysis_path.exists():
        visual_analysis = load_json(visual_analysis_path)
        visual_profile_path = ROOT / "profiles" / "visual" / "notepadpp-visual-profile.json"
        visual_profile = load_json(visual_profile_path) if visual_profile_path.exists() else {}
        visual_result = visual_classify(visual_analysis, visual_profile)
    else:
        visual_result = {
            "primary_class": "VISUAL_INCONCLUSIVE",
            "findings": [],
            "confidence": "LOW",
        }

    # Route to final blocker category
    router = determine_blocker_category(artifacts_dir, compat_result, visual_result, stderr_text, exit_code)

    golden_diff_json = VISUAL_DIR / "crossover-golden" / "notepad-vs-our-golden-diff.json"
    golden_diff_md = VISUAL_DIR / "crossover-golden" / "notepad-vs-our-golden-diff.md"
    golden_diff = {}
    if golden_diff_json.exists():
        try:
            golden_diff = load_json(golden_diff_json)
        except Exception:
            golden_diff = {}

    result = {
        "app_profile_path": str(app_profile_path),
        "selected_run_dir": str(artifacts_dir),
        "artifact_sources": {
            "latest_run_dir": str(artifacts_dir),
            "compat_learning_db": str(COMPAT_DIR / "notepadpp-lessons.json"),
            "visual_profile": str(ROOT / "profiles" / "visual" / "notepadpp-visual-profile.json"),
            "probes_registry": str(ROOT / "profiles" / "probes" / "probes.json"),
        },
        "enabled_classifiers": [
            "tools/compat_learning/classify_app_run.py",
            "tools/visual_regression/classify_visual_state.py",
        ],
        "probe_plan": profile.get("probes_to_run", []),
        "current_stage": _determine_stage(compat_result, profile),
        "current_blocker": router["current_blocker"],
        "blocker_category": router["blocker_category"],
        "confidence": router["confidence"],
        "next_probe": router["next_probe"],
        "next_codex_fixpack": None,
        "safe_to_touch_runtime": router["safe_to_touch_runtime"],
        "safety_rules": router["forbidden_actions"],
        "evidence_paths": {
            "compat_classification_json": str(COMPAT_DIR / "latest-classification.json") if (COMPAT_DIR / "latest-classification.json").exists() else None,
            "compat_classification_md": str(COMPAT_DIR / "LATEST-CLASSIFICATION.md") if (COMPAT_DIR / "LATEST-CLASSIFICATION.md").exists() else None,
            "visual_classification_json": str(VISUAL_DIR / "latest-visual-classification.json") if (VISUAL_DIR / "latest-visual-classification.json").exists() else None,
            "visual_classification_md": str(VISUAL_DIR / "LATEST-VISUAL-CLASSIFICATION.md") if (VISUAL_DIR / "LATEST-VISUAL-CLASSIFICATION.md").exists() else None,
            "visual_analysis_json": str(VISUAL_DIR / "latest-visual-analysis.json") if (VISUAL_DIR / "latest-visual-analysis.json").exists() else None,
            "visual_analysis_md": str(VISUAL_DIR / "LATEST-VISUAL-ANALYSIS.md") if (VISUAL_DIR / "LATEST-VISUAL-ANALYSIS.md").exists() else None,
            "golden_diff_json": str(golden_diff_json) if golden_diff_json.exists() else None,
            "golden_diff_md": str(golden_diff_md) if golden_diff_md.exists() else None,
            "codex_fixpack_md": str(COMPAT_DIR / "CODEX-NEXT-FIXPACK.md") if (COMPAT_DIR / "CODEX-NEXT-FIXPACK.md").exists() else None,
            "codex_next_task_md": None,
        },
        "generated_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "compatibility": {
            "primary_category": compat_result.get("primary_category"),
            "findings": compat_result.get("findings", []),
            "confidence": compat_result.get("confidence"),
        },
        "visual": {
            "primary_class": visual_result.get("primary_class"),
            "findings": visual_result.get("findings", []),
            "confidence": visual_result.get("confidence"),
            "likely_root_layer": visual_result.get("likely_root_layer"),
        },
        "golden_diff": {
            "overall": golden_diff.get("overall"),
            "checks": golden_diff.get("checks", []),
            "golden": golden_diff.get("golden"),
            "current": golden_diff.get("current"),
        },
        "recommended_codex_action": router["recommended_codex_action"],
        "forbidden_actions": router["forbidden_actions"],
        "acceptance_criteria": router["acceptance_criteria"],
        "rollback_rules": router["rollback_rules"],
    }

    return result


def _determine_stage(compat_result: dict, profile: dict) -> str:
    """Estimate current smoke ladder stage from compat findings."""
    primary = compat_result.get("primary_category", "UNKNOWN")
    findings = compat_result.get("findings", [])

    if primary == "INFRA_WINESERVER":
        return "step_1_launch_process"
    if primary == "INFRA_STALE_BUILD":
        return "step_0_inspect_pe"
    if primary == "ENGINE_UNSUPPORTED_OPCODE" or primary == "ENGINE_MEMORY_FAULT" or primary == "ENGINE_ABI_RETURN_PATH":
        return "step_1_launch_process"
    if primary == "WINE_UI_METRICS":
        return "step_9_ui_metrics"
    if primary == "WINE_COMCTL32_IMAGELIST" or any("toolbar" in f.get("detail", "").lower() for f in findings):
        return "step_8_toolbar_visual"
    if primary == "HELPER_CAPTURE_FALSE_NEGATIVE":
        return "step_8_toolbar_visual"
    if primary == "PRODUCT_APP_SPECIFIC":
        return "step_7_dialogs"

    # Check known good
    known_good = profile.get("known_good_checks", [])
    if known_good and "PASS" in str(known_good):
        return "step_10_clean_exit"

    return "unknown"


def write_outputs(result: dict, out_dir: Path):
    out_dir.mkdir(parents=True, exist_ok=True)
    json_path = out_dir / "latest-configurator-result.json"
    md_path = out_dir / "LATEST-CONFIGURATOR-RESULT.md"

    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2)

    lines = [
        "# AI Configurator Result",
        "",
        f"**App:** `{result.get('app_profile_path')}`",
        f"**Run:** `{result.get('selected_run_dir', 'N/A')}`",
        f"**Stage:** `{result.get('current_stage')}`",
        f"**Generated at:** {result['generated_at']}",
        "",
        "## Blocker",
        "",
        f"- **Category:** `{result['blocker_category']}`",
        f"- **Description:** {result['current_blocker']}",
        f"- **Confidence:** {result['confidence']}",
        f"- **Safe to touch runtime:** {'YES' if result['safe_to_touch_runtime'] else 'NO'}",
        "",
        "## Recommended Action",
        "",
        result["recommended_codex_action"],
        "",
        "## Next Probe",
        "",
        f"`{result['next_probe']}`",
        "",
        "## Compatibility Classification",
        "",
        f"- Primary: `{result['compatibility'].get('primary_category', 'N/A')}`",
        f"- Confidence: {result['compatibility'].get('confidence', 'N/A')}",
        "- Findings:",
    ]
    for f in result["compatibility"].get("findings", []):
        lines.append(f"  - `{f.get('category')}` ({f.get('layer')}): {f.get('detail')}")
    if not result["compatibility"].get("findings"):
        lines.append("  - (none)")
    lines.append("")

    lines.extend([
        "## Visual Classification",
        "",
        f"- Primary: `{result['visual'].get('primary_class', 'N/A')}`",
        f"- Confidence: {result['visual'].get('confidence', 'N/A')}",
        f"- Likely root layer: `{result['visual'].get('likely_root_layer', 'N/A')}`",
        "- Findings:",
    ])
    for f in result["visual"].get("findings", []):
        lines.append(f"  - `{f.get('class')}` ({f.get('layer')}): {f.get('detail')}")
    if not result["visual"].get("findings"):
        lines.append("  - (none)")
    lines.append("")

    lines.extend([
        "## CrossOver Golden Diff",
        "",
        f"- Overall: `{result.get('golden_diff', {}).get('overall', 'N/A')}`",
        f"- Golden: `{result.get('golden_diff', {}).get('golden', 'N/A')}`",
        f"- Current: `{result.get('golden_diff', {}).get('current', 'N/A')}`",
        "- Checks:",
    ])
    gd_checks = result.get("golden_diff", {}).get("checks", [])
    for c in gd_checks:
        lines.append(f"  - `{c.get('element')}`: {c.get('status')} ({'; '.join(c.get('reasons', [])) if c.get('reasons') else 'within thresholds'})")
    if not gd_checks:
        lines.append("  - (none)")
    lines.append("")

    lines.extend([
        "## Safety Rules",
        "",
    ])
    for rule in result.get("safety_rules", []):
        lines.append(f"- {rule}")
    lines.append("")

    lines.extend([
        "## Acceptance Criteria",
        "",
    ])
    for crit in result.get("acceptance_criteria", []):
        lines.append(f"- [ ] {crit}")
    lines.append("")

    lines.extend([
        "## Evidence Paths",
        "",
    ])
    for k, v in result.get("evidence_paths", {}).items():
        if v:
            lines.append(f"- `{k}`: {v}")
    lines.append("")

    with open(md_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def main():
    parser = argparse.ArgumentParser(description="MacRunner AI Configurator MVP v0.1")
    parser.add_argument("--app", required=True, help="Path to app profile JSON")
    parser.add_argument("--artifacts", help="Path to run artifacts directory (auto-detected if omitted)")
    parser.add_argument("--mode", default="full", choices=["full", "classify-only"],
                        help="Run mode: full (default) or classify-only")
    parser.add_argument("--out-dir", default=str(AI_DIR), help="Output directory")
    args = parser.parse_args()

    app_path = Path(args.app)
    if not app_path.exists():
        print(f"ERROR: app profile does not exist: {app_path}", file=sys.stderr)
        sys.exit(1)

    artifacts_dir = Path(args.artifacts) if args.artifacts else None

    result = run_configurator(app_path, artifacts_dir, args.mode)
    write_outputs(result, Path(args.out_dir))

    print("Configurator complete.")
    print(f"  JSON: {args.out_dir}/latest-configurator-result.json")
    print(f"  MD:   {args.out_dir}/LATEST-CONFIGURATOR-RESULT.md")
    print(f"  Category: {result['blocker_category']}")
    print(f"  Confidence: {result['confidence']}")
    print(f"  Safe to touch runtime: {'YES' if result['safe_to_touch_runtime'] else 'NO'}")
    print(f"  Next probe: {result['next_probe']}")

    if result["selected_run_dir"]:
        print(f"  Selected run: {result['selected_run_dir']}")
    else:
        print("  Selected run: NONE (auto-detect failed)")


if __name__ == "__main__":
    main()
