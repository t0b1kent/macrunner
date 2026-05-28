"""Telemetry module for MacRunner AI Configurator (Phase A.8).

High-level API:
    from app.configurator.ai.telemetry import TelemetryRecorder
    recorder = TelemetryRecorder(enabled=True)
    recorder.record_run(pe_hash, prediction, outcome, duration_ms=5000)

Privacy guarantees:
- Opt-in only (enabled=False by default)
- No PII in payloads
- Error detail sanitized before upload
- Local queue with GDPR deletion support
- Edge inference (features never leave device)
"""
from __future__ import annotations

from app.configurator.ai.telemetry.client import TelemetryClient, TelemetryClientConfig
from app.configurator.ai.telemetry.models import (
    AggregatedFeedback,
    ErrorClass,
    Outcome,
    TelemetryEvent,
)
from app.configurator.ai.telemetry.privacy import (
    sanitize_error_detail,
    sanitize_telemetry_event,
    truncate,
)

__all__ = [
    "TelemetryClient",
    "TelemetryClientConfig",
    "TelemetryEvent",
    "AggregatedFeedback",
    "Outcome",
    "ErrorClass",
    "sanitize_error_detail",
    "sanitize_telemetry_event",
    "truncate",
]
