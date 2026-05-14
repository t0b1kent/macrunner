# STATUS control-center block1 real integrations

Status: PASS foundation
Verification: swift build PASS, swift test PASS 94/94

## Implemented

- Anthropic URLSession SSE client, Keychain API-key store, system prompt resource, structured JSON parser, retry/rate-limit shell, and AI fix apply planning.
- Steam Keychain store, VDF parser, local manifest parser, Steam Web API client, cover URL selection, and steam:// launch/install URLs.
- Legendary, gogdl, and Battle.net local/wrapper service foundations.
- Strict profile schema file, hand-rolled validator, profile sync plan, and PE SHA-256 matcher.
- macr-hud executable target, overlay window foundation, HUD socket config, rolling frame buffer, and launch plan.
- Root package-control-center.sh and notarize-control-center.sh stubs with release build, app bundle, DMG, SHA-256, appcast, and optional codesign.

## Remaining external work

- Real OAuth/device flows require provider credentials and CLIs.
- Real Sparkle and PLCrashReporter frameworks are placeholders until vendored.
- Notarization requires paid Apple Developer credentials.
- HUD IPC awaits engine JSON-line producer.
