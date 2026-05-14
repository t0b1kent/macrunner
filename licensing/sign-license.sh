#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 2 ]]; then
  echo "Usage: sign-license.sh <email> <expires_iso8601>" >&2
  exit 2
fi
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KEY="$ROOT/licensing/signing.key"
PAYLOAD="$1|$2"
TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT
printf '%s' "$PAYLOAD" > "$TMP"
SIG=$(openssl pkeyutl -rawin -sign -inkey "$KEY" -in "$TMP" | xxd -p -c 512)
printf '%s|%s\n' "$PAYLOAD" "$SIG"
