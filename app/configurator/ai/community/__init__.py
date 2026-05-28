"""MacRunner Community Sharing Module (Phase A.6).

Provides:
- Profile upload (contribute) with automated validation
- Profile download (fetch) with conflict resolution
- Trust score computation
- Privacy/GDPR-compliant PII stripping
- Offline-safe HTTP client

Usage:
    from app.configurator.ai.community import CommunityClient, contribute_profile

    client = CommunityClient(contributor_id="anon_abc123")
    result = client.contribute(pe_hash, profile_json, consent=True)
    fetch = client.fetch(pe_hash, macos_version_major=15)
"""

from __future__ import annotations

from typing import Any

from app.configurator.ai.community.client import CommunityClient
from app.configurator.ai.community.conflict import (
    RankedProfile,
    build_fallback_reasons,
    resolve_conflicts,
    select_best_profile,
)
from app.configurator.ai.community.models import (
    CommunityProfile,
    ContributionRequest,
    ContributionResult,
    FetchRequest,
    FetchResult,
    ReportReason,
    ReportRequest,
    SuccessSignal,
    TrustTier,
    derive_contributor_id,
    generate_anonymous_id,
)
from app.configurator.ai.community.privacy import (
    compute_pe_hash,
    serialize_for_upload,
    strip_pii,
    validate_no_pii,
)
from app.configurator.ai.community.trust import (
    ContributorStats,
    compute_profile_trust,
    compute_trust_score,
    trust_tier,
)
from app.configurator.ai.community.validation import (
    ValidationResult,
    validate_profile,
)


__all__ = [
    # Client
    "CommunityClient",
    # Models
    "CommunityProfile",
    "ContributionRequest",
    "ContributionResult",
    "FetchRequest",
    "FetchResult",
    "ReportRequest",
    "ReportReason",
    "SuccessSignal",
    "TrustTier",
    "derive_contributor_id",
    "generate_anonymous_id",
    # Privacy
    "strip_pii",
    "validate_no_pii",
    "serialize_for_upload",
    "compute_pe_hash",
    # Validation
    "validate_profile",
    "ValidationResult",
    # Trust
    "compute_trust_score",
    "compute_profile_trust",
    "trust_tier",
    "ContributorStats",
    # Conflict
    "resolve_conflicts",
    "select_best_profile",
    "RankedProfile",
    "build_fallback_reasons",
]


def contribute_profile(
    client: CommunityClient,
    pe_hash: str,
    profile_json: dict[str, Any],
    *,
    consent: bool = False,
    macos_version_major: int | None = None,
) -> ContributionResult:
    """High-level wrapper: validate + strip PII + upload.

    Returns a ContributionResult with detailed status.
    """
    # 1. Pre-flight validation
    validation = validate_profile(profile_json)
    if not validation.passed:
        return ContributionResult(
            accepted=False,
            reason=f"Validation failed: {'; '.join(validation.errors)}",
        )

    # 2. Privacy: strip PII
    cleaned = strip_pii(profile_json)
    if not validate_no_pii(cleaned):
        return ContributionResult(
            accepted=False,
            reason="PII detected after sanitization",
        )

    # 3. Upload
    return client.contribute(
        pe_hash=pe_hash,
        profile_json=cleaned,
        macos_version_major=macos_version_major,
        consent=consent,
    )
