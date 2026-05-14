#!/usr/bin/env bash
set -euo pipefail
cat <<'NOTE'
Notarization is intentionally a paid-Apple-Developer path stub.
When credentials are available, run:
  xcrun notarytool submit dist/MacRunner-0.3.0.dmg --apple-id <apple-id> --team-id <team-id> --password <app-specific-password> --wait
Then staple:
  xcrun stapler staple dist/MacRunner-0.3.0.dmg
NOTE
