# STATUS control-center block3 commercial

Status: PASS foundation
Verification: swift build PASS, swift test PASS 94/94

## Implemented

- licensing/feature-gates.json and public-key metadata.
- LicenseStore with Keychain persistence, 14-day trial state, local format validation, detached Ed25519 verify helper, and feature-gate checks.
- Settings license activation UI plus Buy License button.
- PurchaseService opens the hosted lifetime purchase URL and includes a gated StoreKit 2 plan.
- TelemetryService sanitizes home paths, usernames, and email-like strings; uploads only when explicitly enabled.

## Remaining external work

- Production license keys need a real Ed25519 key format/signing pipeline.
- Online activation/revocation endpoint remains a stub.
- PLCrashReporter upload path is represented by controller plumbing until the framework is vendored.
