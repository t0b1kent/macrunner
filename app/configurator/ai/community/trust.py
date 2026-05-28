"""Trust score computation for community contributors (Phase A.6).

Per ADR-A004 §2 trust score formula:
  score = base_score
    + 0.2 * working_profiles  (capped at 1.0)
    + 0.1 * age_months        (capped at 0.5)
    + 0.3 * success_rate
    - 0.5 * reports
    - 1.0 * spam_flag

Range: 0.0 (untrusted) → 2.0+ (expert)
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone

from app.configurator.ai.community.models import CommunityProfile, TrustTier


_BASE_SCORE = 0.5
_WORKING_PROFILE_COEFF = 0.2
_WORKING_PROFILE_CAP = 1.0
_AGE_MONTH_COEFF = 0.1
_AGE_MONTH_CAP = 0.5
_SUCCESS_RATE_COEFF = 0.3
_REPORT_PENALTY = 0.5
_SPAM_PENALTY = 1.0


def compute_trust_score(
    working_profiles: int = 0,
    age_months: float = 0.0,
    success_count: int = 0,
    download_count: int = 0,
    report_count: int = 0,
    spam_flag: bool = False,
) -> float:
    """Compute a contributor trust score from raw metrics.

    Args:
        working_profiles: Number of prior profiles confirmed working.
        age_months: Account age in months.
        success_count: Number of user-reported successes.
        download_count: Total downloads (used to normalize success_rate).
        report_count: Number of user-reported breakages.
        spam_flag: True if manually flagged as spam.
    """
    score = _BASE_SCORE

    # Working profile bonus (capped)
    score += min(working_profiles * _WORKING_PROFILE_COEFF, _WORKING_PROFILE_CAP)

    # Maturity bonus (capped)
    score += min(age_months * _AGE_MONTH_COEFF, _AGE_MONTH_CAP)

    # Success rate component
    total_signals = success_count + report_count
    if total_signals > 0:
        success_rate = success_count / total_signals
    else:
        success_rate = 0.0
    score += success_rate * _SUCCESS_RATE_COEFF

    # Penalties
    score -= report_count * _REPORT_PENALTY
    if spam_flag:
        score -= _SPAM_PENALTY

    # Floor at 0.0
    return max(0.0, round(score, 3))


def compute_profile_trust(profile: CommunityProfile) -> float:
    """Derive trust score directly from a CommunityProfile's fields."""
    # Estimate account age from creation date
    try:
        created = datetime.fromisoformat(profile.created_at.replace("Z", "+00:00"))
        age_months = (datetime.now(timezone.utc) - created).days / 30.0
    except (ValueError, TypeError):
        age_months = 0.0

    return compute_trust_score(
        working_profiles=profile.success_count,  # proxy
        age_months=age_months,
        success_count=profile.success_count,
        download_count=profile.download_count,
        report_count=profile.report_count,
        spam_flag=profile.moderation_status == "rejected",
    )


def trust_tier(score: float) -> TrustTier:
    """Classify a trust score into a tier."""
    if score >= 1.8:
        return TrustTier.EXPERT
    if score >= 1.5:
        return TrustTier.TRUSTED
    if score >= 1.0:
        return TrustTier.TRUSTED
    if score >= 0.5:
        return TrustTier.PROBATION
    return TrustTier.UNTRUSTED


@dataclass(frozen=True)
class ContributorStats:
    """Aggregated stats for a contributor used in leaderboards / rate limit decisions."""

    contributor_id: str
    total_uploads: int = 0
    approved_profiles: int = 0
    rejected_profiles: int = 0
    total_success_count: int = 0
    total_report_count: int = 0
    trust_score: float = 0.0
    tier: TrustTier = TrustTier.UNTRUSTED
