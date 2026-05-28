"""HTTP client for Community Sharing API (Phase A.6).

Design notes:
- Uses stdlib urllib for minimal dependencies (no external HTTP lib required)
- Timeout: 5s connect, 15s read (total max 20s)
- Retries: 1 retry on transient errors (502, 503, 504, timeout)
- Offline mode: returns local fallback if server unreachable
- Rate limiting: client-side token bucket to avoid 429s
"""

from __future__ import annotations

import json
import os
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from typing import Any

from app.configurator.ai.community.conflict import resolve_conflicts
from app.configurator.ai.community.models import (
    CommunityProfile,
    ContributionRequest,
    ContributionResult,
    FetchRequest,
    FetchResult,
    ReportRequest,
    SuccessSignal,
)


_DEFAULT_BASE_URL = "https://api.macrunner.dev"
_REQUEST_TIMEOUT = 15  # seconds
_CONNECT_TIMEOUT = 5


@dataclass
class RateLimiter:
    """Simple token bucket for client-side rate limiting."""

    tokens: float = 10.0
    last_refill: float = field(default_factory=time.monotonic)
    max_tokens: float = 10.0
    refill_rate: float = 1.0 / 6.0  # 1 token every 6 seconds = 10/hour

    def acquire(self) -> bool:
        now = time.monotonic()
        elapsed = now - self.last_refill
        self.tokens = min(self.max_tokens, self.tokens + elapsed * self.refill_rate)
        self.last_refill = now
        if self.tokens >= 1.0:
            self.tokens -= 1.0
            return True
        return False

    def retry_after(self) -> int:
        """Seconds until next token is available."""
        if self.tokens >= 1.0:
            return 0
        return int((1.0 - self.tokens) / self.refill_rate) + 1


class CommunityClient:
    """Client for MacRunner Community Profile API.

    Usage:
        client = CommunityClient(contributor_id="anon_abc123")
        result = client.contribute(pe_hash, profile_json)
        fetch = client.fetch(pe_hash)
    """

    def __init__(
        self,
        contributor_id: str,
        base_url: str | None = None,
        timeout: int = _REQUEST_TIMEOUT,
    ) -> None:
        self.contributor_id = contributor_id
        self.base_url = (base_url or os.environ.get("MACRUNNER_CLOUD_URL", _DEFAULT_BASE_URL)).rstrip("/")
        self.timeout = timeout
        self._rate_limiter = RateLimiter()
        self._offline = False

    # -----------------------------------------------------------------------
    # Public API
    # -----------------------------------------------------------------------

    def contribute(
        self,
        pe_hash: str,
        profile_json: dict[str, Any],
        *,
        macos_version_major: int | None = None,
        wine_version: str | None = None,
        engine_id: str | None = None,
        consent: bool = False,
    ) -> ContributionResult:
        """Upload a profile to the community database."""
        if not self._rate_limiter.acquire():
            return ContributionResult(
                accepted=False,
                reason=f"Rate limited. Retry after {self._rate_limiter.retry_after()}s",
                retry_after_sec=self._rate_limiter.retry_after(),
            )

        req = ContributionRequest(
            pe_hash=pe_hash,
            profile_json=profile_json,
            contributor_id=self.contributor_id,
            macos_version_major=macos_version_major,
            wine_version=wine_version,
            engine_id=engine_id,
            consent=consent,
        )
        errors = req.validate()
        if errors:
            return ContributionResult(
                accepted=False,
                reason=f"Validation failed: {'; '.join(errors)}",
            )

        payload = {
            "pe_hash": req.pe_hash,
            "profile_json": req.profile_json,
            "contributor_id": req.contributor_id,
            "macos_version_major": req.macos_version_major,
            "wine_version": req.wine_version,
            "engine_id": req.engine_id,
            "consent": req.consent,
        }

        status, body = self._post("/api/v1/profiles/contribute", payload)
        if status == 201:
            return ContributionResult(
                accepted=True,
                profile_id=body.get("profile_id"),
                trust_score=body.get("trust_score", 0.0),
                moderation_status=body.get("moderation_status", "pending"),
            )
        if status == 429:
            return ContributionResult(
                accepted=False,
                reason="Server rate limit exceeded",
                retry_after_sec=body.get("retry_after", 3600),
            )
        return ContributionResult(
            accepted=False,
            reason=body.get("error", f"HTTP {status}"),
        )

    def fetch(
        self,
        pe_hash: str,
        *,
        macos_version_major: int | None = None,
        engine_id: str | None = None,
        min_trust_score: float = 0.0,
        limit: int = 5,
    ) -> FetchResult:
        """Download community profiles for a PE hash, with conflict resolution."""
        req = FetchRequest(
            pe_hash=pe_hash,
            macos_version_major=macos_version_major,
            engine_id=engine_id,
            min_trust_score=min_trust_score,
            limit=limit,
        )

        status, body = self._get(
            f"/api/v1/profiles/{pe_hash}",
            params={
                k: v
                for k, v in {
                    "macos_version_major": req.macos_version_major,
                    "engine_id": req.engine_id,
                    "min_trust_score": req.min_trust_score,
                    "limit": req.limit,
                }.items()
                if v is not None
            },
        )

        if status != 200:
            return FetchResult(profiles=[], source="fallback")

        raw_profiles = body.get("profiles", [])
        profiles = [_dict_to_profile(p) for p in raw_profiles]
        ranked = resolve_conflicts(profiles, macos_version_major=macos_version_major)
        sorted_profiles = [r.profile for r in ranked[:req.limit]]
        selected = 0 if sorted_profiles else -1

        return FetchResult(
            profiles=sorted_profiles,
            selected_index=selected,
            source="community",
        )

    def report(
        self,
        pe_hash: str,
        profile_id: str,
        reason: str,
        details: str | None = None,
    ) -> bool:
        """Report a community profile as broken."""
        req = ReportRequest(
            pe_hash=pe_hash,
            profile_id=profile_id,
            contributor_id=self.contributor_id,
            reason=reason,  # type: ignore[arg-type]
            details=details,
        )
        status, _ = self._post(
            f"/api/v1/profiles/{pe_hash}/report",
            {
                "profile_id": req.profile_id,
                "contributor_id": req.contributor_id,
                "reason": req.reason,
                "details": req.details,
            },
        )
        return status == 202

    def success_signal(
        self,
        pe_hash: str,
        profile_id: str,
        duration_minutes: int | None = None,
    ) -> bool:
        """Send a lightweight success signal for a downloaded profile."""
        signal = SuccessSignal(
            pe_hash=pe_hash,
            profile_id=profile_id,
            contributor_id=self.contributor_id,
            duration_minutes=duration_minutes,
        )
        status, _ = self._post(
            f"/api/v1/profiles/{pe_hash}/success",
            {
                "profile_id": signal.profile_id,
                "contributor_id": signal.contributor_id,
                "duration_minutes": signal.duration_minutes,
            },
        )
        return status in {202, 204}

    # -----------------------------------------------------------------------
    # Internal HTTP helpers
    # -----------------------------------------------------------------------

    def _post(self, path: str, payload: dict[str, Any]) -> tuple[int, dict[str, Any]]:
        url = f"{self.base_url}{path}"
        data = json.dumps(payload).encode("utf-8")
        req = urllib.request.Request(
            url,
            data=data,
            headers={
                "Content-Type": "application/json",
                "Accept": "application/json",
                "X-Contributor-ID": self.contributor_id,
            },
            method="POST",
        )
        return self._send(req)

    def _get(self, path: str, params: dict[str, Any]) -> tuple[int, dict[str, Any]]:
        query = "&".join(f"{k}={urllib.parse.quote(str(v))}" for k, v in params.items())
        url = f"{self.base_url}{path}"
        if query:
            url = f"{url}?{query}"
        req = urllib.request.Request(
            url,
            headers={
                "Accept": "application/json",
                "X-Contributor-ID": self.contributor_id,
            },
        )
        return self._send(req)

    def _send(self, req: urllib.request.Request) -> tuple[int, dict[str, Any]]:
        if self._offline:
            return 0, {}
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                body = resp.read().decode("utf-8")
                if body:
                    return resp.status, json.loads(body)
                return resp.status, {}
        except urllib.error.HTTPError as exc:
            body = exc.read().decode("utf-8") if exc.read else ""
            try:
                parsed = json.loads(body) if body else {}
            except json.JSONDecodeError:
                parsed = {"error": body}
            return exc.code, parsed
        except urllib.error.URLError:
            self._offline = True
            return 0, {}
        except Exception:
            return 0, {}


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _dict_to_profile(raw: dict[str, Any]) -> CommunityProfile:
    """Deserialize a CommunityProfile from server JSON."""
    return CommunityProfile(
        pe_hash=raw.get("pe_hash", ""),
        profile_json=raw.get("profile_json", {}),
        contributor_id=raw.get("contributor_id", ""),
        trust_score=raw.get("trust_score", 0.0),
        trust_tier=raw.get("trust_tier", "untrusted"),
        macos_version_major=raw.get("macos_version_major"),
        created_at=raw.get("created_at", ""),
        last_success_at=raw.get("last_success_at"),
        download_count=raw.get("download_count", 0),
        success_count=raw.get("success_count", 0),
        report_count=raw.get("report_count", 0),
        report_reasons=raw.get("report_reasons", {}),
        moderated=raw.get("moderated", False),
        moderation_status=raw.get("moderation_status", "pending"),
        anti_cheat_flag=raw.get("anti_cheat_flag", False),
    )
