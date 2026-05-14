# STATUS control-center block2 features

Status: PASS foundation
Verification: swift build PASS, swift test PASS 94/94

## Implemented

- RU/EN Localizable.strings resources and localization lint script.
- .mrb bottle archive encryption/decryption with CryptoKit AES-256-GCM and manifest metadata.
- Translation cache stats and pre-warm/share planning under cache/translation.
- EngineEnv resource resolution preferring bundled engine/hyperbridge resources with dev fallback to MACRUNNER_ROOT.
- Resource-backed Markdown help center with searchable SwiftUI renderer and Help tab.
- UpdateChannel model and stable/beta/nightly appcast URL selection.

## Remaining external work

- Existing legacy UI strings are not fully converted to LocalizedStringKey.
- .mrb service currently encrypts manifest/file-list payload; full ZIP restore wizard is next.
- Pre-warm measurement depends on live HyperBridge cache metrics from Codex #1.
