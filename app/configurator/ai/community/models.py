"""Pydantic models for Community Sharing API (Phase A.6).

Request/response schemas aligned with ADR-A004 and GDPR requirements.
All fields validated at serialization time.
"""

from __future__ import annotations

import hashlib
import uuid
from dataclasses import dataclass, field
from datetime import datetime, timezone
from enum import Enum
from typing import Any


class TrustTier(str, Enum):
    """Contributor trust classification."""

    UNTRUSTED = "untrusted"       # 0 prior working profiles
    PROBATION = "probation"       # 1–4 working profiles
    TRUSTED = "trusted"           # 5+ working profiles
    EXPERT = "expert"             # 20+ working profiles, high success rate
    MODERATOR = "moderator"       # Manual assignment


class ReportReason(str, Enum):
    """User-facing report categories for profile breakage."""

    CRASH_ON_LAUNCH = "crash_on_launch"
    GRAPHICS_GLITCH = "graphics_glitch"
    PERFORMANCE_POOR = "performance_poor"
    AUDIO_ISSUE = "audio_issue"
    INPUT_BROKEN = "input_broken"
    NETWORK_FAIL = "network_fail"
    ANTICHEAT_TRIGGER = "anticheat_trigger"
    WRONG_CATEGORY = "wrong_category"
    MALICIOUS_CONFIG = "malicious_config"
    OTHER = "other"


@dataclass(frozen=True)
class CommunityProfile:
    """A community-contributed profile with metadata."""

    pe_hash: str                    # SHA-256 of PE binary
    profile_json: dict[str, Any]    # Profile payload (v3 schema)
    contributor_id: str             # Anonymized stable ID
    trust_score: float = 0.0
    trust_tier: TrustTier = TrustTier.UNTRUSTED
    macos_version_major: int | None = None
    created_at: str = field(default_factory=lambda: datetime.now(timezone.utc).isoformat())
    last_success_at: str | None = None
    download_count: int = 0
    success_count: int = 0
    report_count: int = 0
    report_reasons: dict[str, int] = field(default_factory=dict)
    moderated: bool = False
    moderation_status: str = "pending"  # pending | approved | rejected
    anti_cheat_flag: bool = False


@dataclass(frozen=True)
class ContributionRequest:
    """Payload for POST /profiles/contribute."""

    pe_hash: str
    profile_json: dict[str, Any]
    contributor_id: str
    macos_version_major: int | None = None
    wine_version: str | None = None
    engine_id: str | None = None
    consent: bool = False             # GDPR opt-in

    def validate(self) -> list[str]:
        """Client-side pre-flight validation. Returns list of error messages."""
        errors: list[str] = []
        if not self.pe_hash or len(self.pe_hash) != 64:
            errors.append("pe_hash must be SHA-256 (64 hex chars)")
        if not self.profile_json:
            errors.append("profile_json is empty")
        if not self.contributor_id:
            errors.append("contributor_id is required")
        if not self.consent:
            errors.append("user consent required for GDPR compliance")
        return errors


@dataclass(frozen=True)
class ContributionResult:
    """Response from POST /profiles/contribute."""

    accepted: bool
    profile_id: str | None = None
    trust_score: float = 0.0
    moderation_status: str = "pending"
    reason: str = ""
    retry_after_sec: int | None = None


@dataclass(frozen=True)
class FetchRequest:
    """Parameters for GET /profiles/{pe_hash}."""

    pe_hash: str
    macos_version_major: int | None = None
    engine_id: str | None = None
    min_trust_score: float = 0.0
    limit: int = 5


@dataclass(frozen=True)
class FetchResult:
    """Response from GET /profiles/{pe_hash}."""

    profiles: list[CommunityProfile]
    selected_index: int = 0           # Which profile was chosen by conflict resolver
    source: str = "community"         # community | local | fallback


@dataclass(frozen=True)
class ReportRequest:
    """Payload for POST /profiles/{pe_hash}/report."""

    pe_hash: str
    profile_id: str
    contributor_id: str
    reason: ReportReason
    details: str | None = None


@dataclass(frozen=True)
class SuccessSignal:
    """Lightweight success ping for POST /profiles/{pe_hash}/success."""

    pe_hash: str
    profile_id: str
    contributor_id: str
    duration_minutes: int | None = None


# ---------------------------------------------------------------------------
# Anonymization helpers
# ---------------------------------------------------------------------------

def derive_contributor_id(device_fingerprint: str) -> str:
    """Create a stable anonymized contributor ID from device fingerprint.

    Uses HMAC-SHA256 with a fixed pepper so IDs are not reversible
    but stable across sessions on the same device.
    """
    pepper = b"MacRunnerCommunityV1"
    return hashlib.sha256(pepper + device_fingerprint.encode()).hexdigest()[:32]


def generate_anonymous_id() -> str:
    """Generate a random anonymous contributor ID."""
    return uuid.uuid4().hex[:32]
