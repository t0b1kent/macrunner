"""Telemetry models for MacRunner AI Configurator (Phase A.8).

Privacy-first design: all fields are anonymized or aggregated.
No PII, no paths, no user identifiers.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime, timezone
from enum import Enum
from typing import Any


class Outcome(str, Enum):
    """Canonical run outcome."""

    SUCCESS = "success"
    CRASH = "crash"
    FREEZE = "freeze"
    ERROR = "error"
    TIMEOUT = "timeout"
    USER_CANCELLED = "user_cancelled"
    UNKNOWN = "unknown"


class ErrorClass(str, Enum):
    """High-level error classification from stderr parser."""

    NONE = "none"
    MISSING_DLL = "missing_dll"
    UNSUPPORTED_OPCODE = "unsupported_opcode"
    HB_RUNTIME_FAIL = "hb_runtime_fail"
    HB_FAULT = "hb_fault"
    OLE_MARSHAL_FAIL = "ole_marshal_fail"
    KEYBOARD_LOCALE = "keyboard_locale"
    VIRTUAL_SETUP_EXCEPTION = "virtual_setup_exception"
    RPCSS_FAIL = "rpcss_fail"
    ANTICHEAT_BLOCK = "anticheat_block"
    GRAPHICS_INIT_FAIL = "graphics_init_fail"
    OTHER = "other"


@dataclass
class TelemetryEvent:
    """A single anonymized telemetry event."""

    pe_hash: str
    category: str
    predicted_category: str
    graphics_api: str
    predicted_graphics_api: str
    lane: str
    predicted_lane: str
    confidence: float
    outcome: Outcome
    error_class: ErrorClass = ErrorClass.NONE
    error_detail: str = ""
    duration_ms: int = 0
    macos_version_major: int = 0
    wine_version: str = ""
    engine_id: str = ""
    timestamp: str = field(default_factory=lambda: datetime.now(timezone.utc).isoformat())
    schema_version: int = 1

    def to_dict(self) -> dict[str, Any]:
        return {
            "schema_version": self.schema_version,
            "pe_hash": self.pe_hash,
            "category": self.category,
            "predicted_category": self.predicted_category,
            "graphics_api": self.graphics_api,
            "predicted_graphics_api": self.predicted_graphics_api,
            "lane": self.lane,
            "predicted_lane": self.predicted_lane,
            "confidence": round(self.confidence, 4),
            "outcome": self.outcome.value,
            "error_class": self.error_class.value,
            "error_detail": self.error_detail,
            "duration_ms": self.duration_ms,
            "macos_version_major": self.macos_version_major,
            "wine_version": self.wine_version,
            "engine_id": self.engine_id,
            "timestamp": self.timestamp,
        }

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "TelemetryEvent":
        return cls(
            pe_hash=d["pe_hash"],
            category=d.get("category", ""),
            predicted_category=d.get("predicted_category", ""),
            graphics_api=d.get("graphics_api", ""),
            predicted_graphics_api=d.get("predicted_graphics_api", ""),
            lane=d.get("lane", ""),
            predicted_lane=d.get("predicted_lane", ""),
            confidence=d.get("confidence", 0.0),
            outcome=Outcome(d.get("outcome", "unknown")),
            error_class=ErrorClass(d.get("error_class", "none")),
            error_detail=d.get("error_detail", ""),
            duration_ms=d.get("duration_ms", 0),
            macos_version_major=d.get("macos_version_major", 0),
            wine_version=d.get("wine_version", ""),
            engine_id=d.get("engine_id", ""),
            timestamp=d.get("timestamp", datetime.now(timezone.utc).isoformat()),
            schema_version=d.get("schema_version", 1),
        )


@dataclass
class AggregatedFeedback:
    """Aggregated feedback for a single PE hash."""

    pe_hash: str
    total_runs: int = 0
    success_count: int = 0
    crash_count: int = 0
    freeze_count: int = 0
    error_count: int = 0
    timeout_count: int = 0
    avg_confidence: float = 0.0
    most_common_error: ErrorClass = ErrorClass.NONE
    last_run_at: str = ""
    validated: bool = False

    def to_dict(self) -> dict[str, Any]:
        return {
            "pe_hash": self.pe_hash,
            "total_runs": self.total_runs,
            "success_count": self.success_count,
            "crash_count": self.crash_count,
            "freeze_count": self.freeze_count,
            "error_count": self.error_count,
            "timeout_count": self.timeout_count,
            "avg_confidence": round(self.avg_confidence, 4),
            "most_common_error": self.most_common_error.value,
            "last_run_at": self.last_run_at,
            "validated": self.validated,
        }

    @property
    def success_rate(self) -> float:
        return self.success_count / self.total_runs if self.total_runs else 0.0

    @property
    def should_trigger_retrain(self) -> bool:
        """True if we have enough new data to warrant retraining."""
        return self.total_runs >= 10 and self.success_rate > 0.5
