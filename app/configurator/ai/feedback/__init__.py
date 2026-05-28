"""Feedback loop for MacRunner AI Configurator (Phase A.8).

Aggregates telemetry events per PE hash, triggers retraining when
significant new data arrives, and implements active learning for
low-confidence predictions.

Usage:
    from app.configurator.ai.feedback import FeedbackLoop
    loop = FeedbackLoop()
    loop.ingest(event)  # TelemetryEvent
    if loop.should_retrain():
        loop.trigger_retrain()
    if loop.should_prompt_user(pe_hash, confidence):
        loop.prompt_user(pe_hash)
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from app.configurator.ai.telemetry.models import (
    AggregatedFeedback,
    ErrorClass,
    Outcome,
    TelemetryEvent,
)


# Tunable thresholds
_MIN_RUNS_FOR_VALIDATION = 10
_MIN_SUCCESS_RATE_FOR_VALIDATION = 0.5
_LOW_CONFIDENCE_THRESHOLD = 0.7
_RETRAIN_MIN_NEW_EVENTS = 20
_RETRAIN_MIN_TIME_SINCE_LAST_SEC = 86400  # 24h


@dataclass
class FeedbackLoopState:
    """Persistent state for the feedback loop."""

    aggregates: dict[str, AggregatedFeedback] = field(default_factory=dict)
    total_events_ingested: int = 0
    last_retrain_timestamp: str = ""
    low_confidence_prompts: dict[str, int] = field(default_factory=dict)
    pending_training_labels: list[dict[str, Any]] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        return {
            "aggregates": {k: v.to_dict() for k, v in self.aggregates.items()},
            "total_events_ingested": self.total_events_ingested,
            "last_retrain_timestamp": self.last_retrain_timestamp,
            "low_confidence_prompts": self.low_confidence_prompts,
            "pending_training_labels": self.pending_training_labels,
        }

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "FeedbackLoopState":
        agg = {}
        for k, v in d.get("aggregates", {}).items():
            af = AggregatedFeedback(pe_hash=k)
            for field_name in ["total_runs", "success_count", "crash_count", "freeze_count",
                               "error_count", "timeout_count", "avg_confidence",
                               "most_common_error", "last_run_at", "validated"]:
                if field_name in v:
                    setattr(af, field_name, v[field_name])
            agg[k] = af
        return cls(
            aggregates=agg,
            total_events_ingested=d.get("total_events_ingested", 0),
            last_retrain_timestamp=d.get("last_retrain_timestamp", ""),
            low_confidence_prompts=d.get("low_confidence_prompts", {}),
            pending_training_labels=d.get("pending_training_labels", []),
        )


class FeedbackLoop:
    """Ingest telemetry, aggregate, and trigger retraining."""

    def __init__(self, state_path: Path | str | None = None) -> None:
        self._state_path = Path(state_path) if state_path else Path.home() / ".macrunner" / "feedback_state.json"
        self._state_path.parent.mkdir(parents=True, exist_ok=True)
        self._state = self._load_state()

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def ingest(self, event: TelemetryEvent) -> None:
        """Ingest a single telemetry event and update aggregates."""
        h = event.pe_hash
        agg = self._state.aggregates.setdefault(h, AggregatedFeedback(pe_hash=h))

        agg.total_runs += 1
        if event.outcome == Outcome.SUCCESS:
            agg.success_count += 1
        elif event.outcome == Outcome.CRASH:
            agg.crash_count += 1
        elif event.outcome == Outcome.FREEZE:
            agg.freeze_count += 1
        elif event.outcome == Outcome.ERROR:
            agg.error_count += 1
        elif event.outcome == Outcome.TIMEOUT:
            agg.timeout_count += 1

        # Update average confidence (Welford-style)
        n = agg.total_runs
        agg.avg_confidence = agg.avg_confidence + (event.confidence - agg.avg_confidence) / n

        # Most common error
        if event.error_class != ErrorClass.NONE:
            agg.most_common_error = event.error_class

        agg.last_run_at = event.timestamp

        # Validate if threshold met
        if agg.total_runs >= _MIN_RUNS_FOR_VALIDATION and agg.success_rate >= _MIN_SUCCESS_RATE_FOR_VALIDATION:
            agg.validated = True

        self._state.total_events_ingested += 1

        # If prediction was wrong, queue as labeled training example
        if event.predicted_category != event.category or event.predicted_graphics_api != event.graphics_api or event.predicted_lane != event.lane:
            self._state.pending_training_labels.append({
                "pe_hash": event.pe_hash,
                "category": event.category,
                "graphics_api": event.graphics_api,
                "lane": event.lane,
                "timestamp": event.timestamp,
            })

        self._save_state()

    def get_aggregate(self, pe_hash: str) -> AggregatedFeedback | None:
        return self._state.aggregates.get(pe_hash)

    def should_retrain(self) -> bool:
        """True if enough new labeled data exists for retraining."""
        from datetime import datetime, timezone

        if len(self._state.pending_training_labels) < _RETRAIN_MIN_NEW_EVENTS:
            return False

        if self._state.last_retrain_timestamp:
            last = datetime.fromisoformat(self._state.last_retrain_timestamp)
            now = datetime.now(timezone.utc)
            if (now - last).total_seconds() < _RETRAIN_MIN_TIME_SINCE_LAST_SEC:
                return False

        return True

    def trigger_retrain(self) -> dict[str, Any]:
        """Mark retraining as triggered and clear pending labels."""
        from datetime import datetime, timezone

        result = {
            "triggered": True,
            "new_labels": len(self._state.pending_training_labels),
            "aggregates": len(self._state.aggregates),
        }
        self._state.last_retrain_timestamp = datetime.now(timezone.utc).isoformat()
        self._state.pending_training_labels = []
        self._save_state()
        return result

    def should_prompt_user(self, pe_hash: str, confidence: float) -> bool:
        """Active learning: prompt user for low-confidence predictions."""
        if confidence >= _LOW_CONFIDENCE_THRESHOLD:
            return False
        # Don't prompt more than 3 times for same hash
        prompts = self._state.low_confidence_prompts.get(pe_hash, 0)
        if prompts >= 3:
            return False
        return True

    def record_user_prompt(self, pe_hash: str) -> None:
        self._state.low_confidence_prompts[pe_hash] = self._state.low_confidence_prompts.get(pe_hash, 0) + 1
        self._save_state()

    def record_user_correction(self, pe_hash: str, category: str, graphics_api: str, lane: str) -> None:
        """User explicitly corrected a prediction — high-value training label."""
        from datetime import datetime, timezone
        self._state.pending_training_labels.append({
            "pe_hash": pe_hash,
            "category": category,
            "graphics_api": graphics_api,
            "lane": lane,
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "source": "user_correction",
        })
        self._save_state()

    def get_all_aggregates(self) -> dict[str, AggregatedFeedback]:
        return dict(self._state.aggregates)

    @property
    def total_events(self) -> int:
        return self._state.total_events_ingested

    @property
    def validated_hashes(self) -> list[str]:
        return [h for h, agg in self._state.aggregates.items() if agg.validated]

    # ------------------------------------------------------------------
    # Persistence
    # ------------------------------------------------------------------

    def _load_state(self) -> FeedbackLoopState:
        if self._state_path.exists():
            try:
                data = json.loads(self._state_path.read_text(encoding="utf-8"))
                return FeedbackLoopState.from_dict(data)
            except (json.JSONDecodeError, OSError):
                pass
        return FeedbackLoopState()

    def _save_state(self) -> None:
        try:
            self._state_path.write_text(
                json.dumps(self._state.to_dict(), indent=2), encoding="utf-8"
            )
        except OSError:
            pass
