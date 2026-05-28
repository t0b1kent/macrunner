#!/bin/bash
# MacRunner Rosetta preflight wrapper.
set -euo pipefail

if ! /usr/bin/arch -x86_64 /usr/bin/true >/dev/null 2>&1; then
    echo "❌ Rosetta 2 is not available."
    echo "   Install it with: softwareupdate --install-rosetta --agree-to-license"
    exit 1
fi

export ROSETTA_ADVERTISE_AVX="${ROSETTA_ADVERTISE_AVX:-1}"

if [ "$#" -eq 0 ]; then
    echo "usage: run-with-rosetta.sh <command> [args...]" >&2
    exit 2
fi

exec /usr/bin/arch -x86_64 "$@"
