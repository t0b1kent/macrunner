# Control Center Master Status

Date: 2026-05-14
Root: /Users/timurtoby/Documents/MacRunner/Main/MacRunner
Scope: app/macr-control-center, profiles, config, docs/STATUS-control-center-*, tools
Protected: engine, wine-fork, scripts/build-wine.sh, scripts/sign-engine.sh, scripts/loop-wineboot.sh

## Summary

Autonomous phases alpha through iota are implemented as a portable Control Center foundation. The app now has a dedicated Best of All Worlds tab, external-disk defaults, local library aggregation, bottle templates, profile loading, HUD telemetry models, offline troubleshoot plumbing, onboarding planning, and packaging boundaries.

## Phase Matrix

| Phase | Area | Status |
| --- | --- | --- |
| alpha | Competitive audit | PASS |
| beta | Unified library aggregator | PASS |
| gamma | Bottle/prefix manager foundation | PASS |
| delta | Profile system | PASS |
| epsilon | Performance HUD foundation | PASS |
| zeta | AI troubleshoot foundation | PASS |
| eta | Liquid Glass polish | PASS |
| theta | Onboarding and first launch | PASS |
| iota | Release packaging plan | PASS |

## Remaining Limitations

- HUD is telemetry/model foundation plus in-app preview, not a global overlay injector.
- AI troubleshoot is offline prompt/classifier plumbing; no network call or API key flow is enabled.
- Library providers are local manifest scanners; authenticated store APIs are intentionally not used.
- Packaging references app-local tooling and excludes engine/Wine assets by contract.

## Phase 2 Shippable Tracker

| Block | Task | Patch | Status |
| --- | --- | --- | --- |
| 1 | AI troubleshoot real API + Keychain | patches/control-center-block1-ai-real.patch | PASS foundation |
| 1 | Steam OAuth + library | patches/control-center-block1-steam.patch | PASS foundation |
| 1 | Epic/GOG/Battle.net providers | patches/control-center-block1-other-stores.patch | PASS foundation |
| 1 | Strict profile validation + community DB | patches/control-center-block1-profiles-strict.patch | PASS foundation |
| 1 | Global HUD overlay injector | patches/control-center-block1-hud-global.patch | PASS foundation |
| 1 | Signed DMG + Sparkle + notarization stub | patches/control-center-block1-packaging.patch | PASS foundation |
| 2 | Multi-language RU/EN | patches/control-center-block2-localization.patch | PASS foundation |
| 2 | Bottle import/export .mrb | patches/control-center-block2-bottle-share.patch | PASS foundation |
| 2 | Translation cache pre-warm UI services | patches/control-center-block2-cache-prewarm.patch | PASS foundation |
| 2 | Bundle bridge into .app resources | patches/control-center-block2-bundle-bridge.patch | PASS foundation |
| 2 | In-app docs/help center | patches/control-center-block2-docs.patch | PASS foundation |
| 2 | Update channels | patches/control-center-block2-channels.patch | PASS foundation |
| 3 | License activation UI | patches/control-center-block3-licensing.patch | PASS foundation |
| 3 | Purchase stub | patches/control-center-block3-purchase-stub.patch | PASS foundation |
| 3 | Telemetry/crash reporter opt-in | patches/control-center-block3-telemetry.patch | PASS foundation |

Phase 2 verification: swift build PASS, swift test PASS 94/94. External production acceptance still requires real Anthropic/Steam/GitHub credentials, Sparkle framework, Apple Developer signing/notarization, and live engine artifacts.
