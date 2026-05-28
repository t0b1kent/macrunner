"""Production Validation Harness for AI Configurator (Phase A.7).

Runs the full PE analysis → XGBoost prediction pipeline on real fixtures
and compares against expected ground truth. Reports accuracy, confidence,
latency, and identifies gaps per tier.
"""

from __future__ import annotations

import json
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from app.configurator.ai.model import ProfilePredictor, _pe_features_to_flat
from app.configurator.ai.pe_analyzer import analyze_pe
from app.configurator.ai.pe_analyzer.features import build_feature_vector


@dataclass
class ValidationCase:
    """A single test case: fixture + expected predictions."""

    path: str
    expected_category: str | None = None
    expected_graphics_api: str | None = None
    expected_lane: str | None = None
    tier: str = "unknown"
    description: str = ""


@dataclass
class ValidationResult:
    """Outcome for a single case."""

    case: ValidationCase
    predicted_category: str
    predicted_api: str
    predicted_lane: str
    confidence: float
    latency_ms: float
    pe_hash: str
    error: str | None = None

    @property
    def category_correct(self) -> bool:
        if self.case.expected_category is None:
            return True
        return self.predicted_category == self.case.expected_category

    @property
    def api_correct(self) -> bool:
        if self.case.expected_graphics_api is None:
            return True
        return self.predicted_api == self.case.expected_graphics_api

    @property
    def lane_correct(self) -> bool:
        if self.case.expected_lane is None:
            return True
        return self.predicted_lane == self.case.expected_lane

    @property
    def all_correct(self) -> bool:
        return self.category_correct and self.api_correct and self.lane_correct


@dataclass
class TierSummary:
    """Aggregated stats for a single tier."""

    tier: str
    total: int = 0
    category_hits: int = 0
    api_hits: int = 0
    lane_hits: int = 0
    full_hits: int = 0
    avg_confidence: float = 0.0
    avg_latency_ms: float = 0.0
    failures: list[str] = field(default_factory=list)

    @property
    def category_acc(self) -> float:
        return self.category_hits / self.total if self.total else 0.0

    @property
    def api_acc(self) -> float:
        return self.api_hits / self.total if self.total else 0.0

    @property
    def lane_acc(self) -> float:
        return self.lane_hits / self.total if self.total else 0.0

    @property
    def full_acc(self) -> float:
        return self.full_hits / self.total if self.total else 0.0


@dataclass
class ValidationReport:
    """Complete validation report across all tiers."""

    results: list[ValidationResult]
    tier_summaries: dict[str, TierSummary]
    overall: TierSummary
    gaps: list[str] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        return {
            "overall": {
                "total": self.overall.total,
                "category_accuracy": round(self.overall.category_acc, 3),
                "api_accuracy": round(self.overall.api_acc, 3),
                "lane_accuracy": round(self.overall.lane_acc, 3),
                "full_accuracy": round(self.overall.full_acc, 3),
                "avg_confidence": round(self.overall.avg_confidence, 3),
                "avg_latency_ms": round(self.overall.avg_latency_ms, 4),
            },
            "tiers": {
                tier: {
                    "total": s.total,
                    "category_accuracy": round(s.category_acc, 3),
                    "api_accuracy": round(s.api_acc, 3),
                    "lane_accuracy": round(s.lane_acc, 3),
                    "full_accuracy": round(s.full_acc, 3),
                    "avg_confidence": round(s.avg_confidence, 3),
                    "avg_latency_ms": round(s.avg_latency_ms, 4),
                    "failures": s.failures,
                }
                for tier, s in self.tier_summaries.items()
            },
            "gaps": self.gaps,
            "cases": [
                {
                    "path": r.case.path,
                    "tier": r.case.tier,
                    "expected_category": r.case.expected_category,
                    "predicted_category": r.predicted_category,
                    "expected_api": r.case.expected_graphics_api,
                    "predicted_api": r.predicted_api,
                    "expected_lane": r.case.expected_lane,
                    "predicted_lane": r.predicted_lane,
                    "confidence": r.confidence,
                    "latency_ms": round(r.latency_ms, 4),
                    "correct": r.all_correct,
                    "error": r.error,
                }
                for r in self.results
            ],
        }

    def to_json(self, indent: int = 2) -> str:
        return json.dumps(self.to_dict(), indent=indent, ensure_ascii=False)


def run_validation(
    cases: list[ValidationCase],
    *,
    strategy: str = "xgboost",
    model_dir: str | None = None,
) -> ValidationReport:
    """Run the validation pipeline on all cases and produce a report."""
    predictor = ProfilePredictor(strategy=strategy, model_dir=model_dir)
    results: list[ValidationResult] = []
    tier_map: dict[str, TierSummary] = {}
    overall = TierSummary(tier="overall")

    for case in cases:
        path = Path(case.path)
        t0 = time.perf_counter()
        result: ValidationResult | None = None

        if not path.exists():
            result = ValidationResult(
                case=case,
                predicted_category="missing",
                predicted_api="missing",
                predicted_lane="missing",
                confidence=0.0,
                latency_ms=0.0,
                pe_hash="",
                error="file not found",
            )
        else:
            try:
                pe_result = analyze_pe(path)
                pe_features = build_feature_vector(pe_result)
                flat = _pe_features_to_flat(pe_features)
                pred_result = predictor.predict(flat)
                profile = predictor.predict_profile_dict(flat)
                latency = (time.perf_counter() - t0) * 1000

                cat = pred_result.predicted_category
                api = profile.get("graphics", {}).get("api", "none")
                lane = profile.get("default_lane", "unknown")

                result = ValidationResult(
                    case=case,
                    predicted_category=cat,
                    predicted_api=api,
                    predicted_lane=lane,
                    confidence=pred_result.confidence,
                    latency_ms=latency,
                    pe_hash=pe_result.sha256,
                )
            except Exception as exc:
                latency = (time.perf_counter() - t0) * 1000
                result = ValidationResult(
                    case=case,
                    predicted_category="error",
                    predicted_api="error",
                    predicted_lane="error",
                    confidence=0.0,
                    latency_ms=latency,
                    pe_hash="",
                    error=str(exc),
                )

        assert result is not None
        results.append(result)

        # Aggregate
        tier = tier_map.setdefault(case.tier, TierSummary(tier=case.tier))
        tier.total += 1
        overall.total += 1
        if result.category_correct:
            tier.category_hits += 1
            overall.category_hits += 1
        if result.api_correct:
            tier.api_hits += 1
            overall.api_hits += 1
        if result.lane_correct:
            tier.lane_hits += 1
            overall.lane_hits += 1
        if result.all_correct:
            tier.full_hits += 1
            overall.full_hits += 1
        tier.avg_confidence += result.confidence
        overall.avg_confidence += result.confidence
        tier.avg_latency_ms += result.latency_ms
        overall.avg_latency_ms += result.latency_ms
        if result.error:
            tier.failures.append(f"{path.name}: {result.error}")
        elif not result.all_correct:
            tier.failures.append(
                f"{path.name}: cat={result.predicted_category}({case.expected_category}), "
                f"api={result.predicted_api}({case.expected_graphics_api}), "
                f"lane={result.predicted_lane}({case.expected_lane})"
            )

    # Normalize averages
    for tier in tier_map.values():
        if tier.total:
            tier.avg_confidence /= tier.total
            tier.avg_latency_ms /= tier.total
    if overall.total:
        overall.avg_confidence /= overall.total
        overall.avg_latency_ms /= overall.total

    # Identify gaps
    gaps: list[str] = []
    if overall.category_acc < 0.8:
        gaps.append(f"category accuracy {overall.category_acc:.2%} below 80% threshold")
    if overall.api_acc < 0.7:
        gaps.append(f"graphics API accuracy {overall.api_acc:.2%} below 70% threshold")
    if overall.lane_acc < 0.7:
        gaps.append(f"lane accuracy {overall.lane_acc:.2%} below 70% threshold")
    if overall.avg_latency_ms > 50:
        gaps.append(f"avg latency {overall.avg_latency_ms:.2f}ms above 50ms threshold")

    # Per-tier gaps
    for tier_name, tier in tier_map.items():
        if tier.full_acc < 0.5:
            gaps.append(f"tier '{tier_name}' full accuracy {tier.full_acc:.2%} below 50%")

    return ValidationReport(
        results=results,
        tier_summaries=tier_map,
        overall=overall,
        gaps=gaps,
    )
