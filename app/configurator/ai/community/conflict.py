"""Conflict resolution for multiple community profiles (Phase A.6).

When a PE hash has multiple uploaded profiles, rank candidates by:
  1. Trust score (highest first)
  2. macOS version match (same major version preferred)
  3. Recency (last success within 30 days preferred)
  4. Download count (social proof, capped)

Per ADR-A004 §2.
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from typing import Any

from app.configurator.ai.community.models import CommunityProfile
from app.configurator.ai.community.trust import compute_profile_trust


# Weights for composite ranking score (tunable).
_WEIGHT_TRUST = 1.0
_WEIGHT_MACOS_MATCH = 0.8
_WEIGHT_RECENCY = 0.6
_WEIGHT_DOWNLOADS = 0.2

_MAX_DOWNLOAD_CAP = 1000  # Prevent popularity bias from dominating


@dataclass(frozen=True)
class RankedProfile:
    """A profile with its computed ranking metadata."""

    profile: CommunityProfile
    trust_score: float
    macos_match: bool
    recent_success: bool
    download_score: float
    composite: float


def resolve_conflicts(
    profiles: list[CommunityProfile],
    *,
    macos_version_major: int | None = None,
) -> list[RankedProfile]:
    """Rank multiple community profiles for the same PE hash.

    Returns sorted list (best first). If profiles is empty, returns empty list.
    """
    if not profiles:
        return []

    ranked: list[RankedProfile] = []
    for profile in profiles:
        trust = compute_profile_trust(profile)

        macos_match = False
        if macos_version_major is not None and profile.macos_version_major is not None:
            macos_match = macos_version_major == profile.macos_version_major

        recent_success = False
        if profile.last_success_at:
            try:
                last = datetime.fromisoformat(profile.last_success_at.replace("Z", "+00:00"))
                recent_success = (datetime.now(timezone.utc) - last) <= timedelta(days=30)
            except (ValueError, TypeError):
                pass

        capped_downloads = min(profile.download_count, _MAX_DOWNLOAD_CAP)
        download_score = capped_downloads / _MAX_DOWNLOAD_CAP

        composite = round(
            _WEIGHT_TRUST * trust
            + _WEIGHT_MACOS_MATCH * (1.0 if macos_match else 0.0)
            + _WEIGHT_RECENCY * (1.0 if recent_success else 0.0)
            + _WEIGHT_DOWNLOADS * download_score,
            4,
        )

        ranked.append(
            RankedProfile(
                profile=profile,
                trust_score=trust,
                macos_match=macos_match,
                recent_success=recent_success,
                download_score=download_score,
                composite=composite,
            )
        )

    # Sort descending by composite score
    ranked.sort(key=lambda r: r.composite, reverse=True)
    return ranked


def select_best_profile(
    profiles: list[CommunityProfile],
    *,
    macos_version_major: int | None = None,
) -> CommunityProfile | None:
    """Pick the single best community profile."""
    ranked = resolve_conflicts(profiles, macos_version_major=macos_version_major)
    if ranked:
        return ranked[0].profile
    return None


def build_fallback_reasons(profile: CommunityProfile) -> list[str]:
    """Human-readable reasons why this profile was selected."""
    reasons: list[str] = []
    trust = compute_profile_trust(profile)
    if trust >= 1.5:
        reasons.append("High contributor trust")
    elif trust >= 1.0:
        reasons.append("Trusted contributor")

    if profile.last_success_at:
        reasons.append("Recently confirmed working")

    if profile.download_count > 50:
        reasons.append(f"Popular ({profile.download_count} downloads)")

    return reasons or ["Community profile (unverified)"]
