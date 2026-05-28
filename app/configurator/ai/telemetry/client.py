"""Telemetry upload client (Phase A.8).

Stdlib-only HTTP client with:
- Local disk queue (offline resilience)
- Batch upload
- Rate limiting
- Privacy sanitization pre-flight
"""
from __future__ import annotations

import json
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from app.configurator.ai.telemetry.models import TelemetryEvent
from app.configurator.ai.telemetry.privacy import sanitize_telemetry_event


_DEFAULT_ENDPOINT = "https://api.macrunner.dev/api/v1/telemetry"
_DEFAULT_BATCH_SIZE = 10
_DEFAULT_FLUSH_INTERVAL_SEC = 300


@dataclass
class TelemetryClientConfig:
    """Configuration for telemetry client."""

    enabled: bool = False
    endpoint: str = _DEFAULT_ENDPOINT
    batch_size: int = _DEFAULT_BATCH_SIZE
    flush_interval_sec: int = _DEFAULT_FLUSH_INTERVAL_SEC
    api_key: str = ""
    contributor_id: str = ""
    max_queue_size: int = 1000
    offline_fallback: bool = True


class TelemetryClient:
    """Upload telemetry events with queuing and privacy guarantees."""

    def __init__(
        self,
        config: TelemetryClientConfig | None = None,
        queue_dir: Path | str | None = None,
    ) -> None:
        self.config = config or TelemetryClientConfig()
        self._queue_dir = Path(queue_dir) if queue_dir else Path.home() / ".macrunner" / "telemetry_queue"
        self._queue_dir.mkdir(parents=True, exist_ok=True)
        self._last_flush = 0.0
        self._offline = False

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def emit(self, event: TelemetryEvent) -> bool:
        """Queue a single telemetry event. Flushes if batch is full."""
        if not self.config.enabled:
            return False

        # Sanitize before any disk/network operation
        sanitized = sanitize_telemetry_event(event.to_dict())

        # Write to queue
        ts = time.time()
        queue_path = self._queue_dir / f"{ts}_{event.pe_hash[:8]}.json"
        try:
            queue_path.write_text(json.dumps(sanitized), encoding="utf-8")
        except OSError:
            return False

        # Flush if batch full or interval elapsed
        pending = len(list(self._queue_dir.glob("*.json")))
        if pending >= self.config.batch_size or (time.time() - self._last_flush) >= self.config.flush_interval_sec:
            self.flush()

        return True

    def flush(self) -> dict[str, Any]:
        """Upload all queued events. Returns summary."""
        if not self.config.enabled:
            return {"uploaded": 0, "failed": 0, "reason": "telemetry disabled"}

        files = sorted(self._queue_dir.glob("*.json"), key=lambda p: p.stat().st_mtime)
        if not files:
            return {"uploaded": 0, "failed": 0}

        uploaded = 0
        failed = 0
        batch: list[dict[str, Any]] = []

        for f in files:
            try:
                event = json.loads(f.read_text(encoding="utf-8"))
                batch.append(event)
                if len(batch) >= self.config.batch_size:
                    if self._upload_batch(batch):
                        uploaded += len(batch)
                    else:
                        failed += len(batch)
                    batch = []
            except (OSError, json.JSONDecodeError):
                failed += 1
                try:
                    f.unlink()
                except OSError:
                    pass

        if batch:
            if self._upload_batch(batch):
                uploaded += len(batch)
            else:
                failed += len(batch)

        self._last_flush = time.time()
        return {"uploaded": uploaded, "failed": failed}

    def delete_all_local(self) -> int:
        """Delete all queued events (GDPR right-to-be-forgotten)."""
        count = 0
        for f in self._queue_dir.glob("*.json"):
            try:
                f.unlink()
                count += 1
            except OSError:
                pass
        return count

    def queue_size(self) -> int:
        return len(list(self._queue_dir.glob("*.json")))

    # ------------------------------------------------------------------
    # Internal
    # ------------------------------------------------------------------

    def _upload_batch(self, batch: list[dict[str, Any]]) -> bool:
        if self._offline and not self.config.offline_fallback:
            return False

        payload = json.dumps({"events": batch, "contributor_id": self.config.contributor_id}).encode("utf-8")
        req = urllib.request.Request(
            self.config.endpoint,
            data=payload,
            headers={
                "Content-Type": "application/json",
                "X-API-Key": self.config.api_key,
            },
            method="POST",
        )

        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                code = resp.getcode()
                if code == 202:
                    # Accepted — delete queue files
                    self._remove_batch_files(batch)
                    self._offline = False
                    return True
                elif code == 429:
                    # Rate limited — keep files for retry
                    self._offline = False
                    return False
                else:
                    self._remove_batch_files(batch)
                    return True
        except urllib.error.HTTPError as exc:
            if exc.code == 429:
                return False
            # Server error — drop batch to avoid infinite retry
            self._remove_batch_files(batch)
            return True
        except urllib.error.URLError:
            self._offline = True
            return False
        except Exception:
            self._offline = True
            return False

    def _remove_batch_files(self, batch: list[dict[str, Any]]) -> None:
        """Delete queue files matching the uploaded batch."""
        hashes = {e.get("pe_hash", "") for e in batch}
        for f in self._queue_dir.glob("*.json"):
            try:
                data = json.loads(f.read_text(encoding="utf-8"))
                if data.get("pe_hash") in hashes:
                    f.unlink()
            except (OSError, json.JSONDecodeError):
                pass
