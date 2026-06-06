#!/bin/bash

# Source project environment if available
if [ -f "./config/env.sh" ]; then
    . ./config/env.sh
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MACRUNNER_ROOT="${MACRUNNER_ROOT:-/Users/timurtoby/Documents/MacRunner/Main/MacRunner}"
UNARC_BIN="/tmp/freearc-build/unarc/unarc"
ARCHIVE_PATH="$SCRIPT_DIR/test_open.arc"
OUT_DIR="/tmp/test_open_extracted"

# Default to run nothing unless specified
RUNG_A=0
RUNG_B=0
RUNG_C=0
RUNG_D=0

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --native|--rung-a)
            RUNG_A=1
            shift
            ;;
        --wine|--rung-b)
            RUNG_B=1
            shift
            ;;
        --rung-c)
            RUNG_C=1
            shift
            ;;
        --rung-d)
            RUNG_D=1
            shift
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--rung-a/--native] [--rung-b/--wine] [--rung-c] [--rung-d]"
            exit 1
            ;;
    esac
done

# If no arguments provided, show help
if [ $RUNG_A -eq 0 ] && [ $RUNG_B -eq 0 ] && [ $RUNG_C -eq 0 ] && [ $RUNG_D -eq 0 ]; then
    echo "No rungs specified. Running default Rung A (Native FreeArc smoke)..."
    RUNG_A=1
fi

if [ $RUNG_A -eq 1 ]; then
    echo "==== Running Rung A: Native FreeArc Smoke ===="
    if [ ! -f "$UNARC_BIN" ]; then
        echo "Error: Native unarc binary not found at $UNARC_BIN"
        echo "Please compile it first."
        exit 1
    fi
    
    # Run python verifier
    python3 "$SCRIPT_DIR/extract_and_verify.py" "$UNARC_BIN" "$ARCHIVE_PATH" "$OUT_DIR"
    EXIT_CODE=$?
    
    if [ $EXIT_CODE -eq 0 ]; then
        echo "Rung A test PASS"
    else
        echo "Rung A test FAIL"
    fi
    exit $EXIT_CODE
fi

if [ $RUNG_B -eq 1 ]; then
    echo "==== Rung B: Inno/NSIS installer (no lolz) ===="
    echo "SKIP: BLOCKED on PE32 Phase 4 (32-bit CPU JIT and WOW64 not live)"
    exit 0
fi

if [ $RUNG_C -eq 1 ]; then
    echo "==== Rung C: FreeArc repack with SREP (no lolz) ===="
    echo "SKIP: BLOCKED on PE32 Phase 4 (CreateProcess and Overlapped Pipes not live)"
    exit 0
fi

if [ $RUNG_D -eq 1 ]; then
    echo "==== Rung D: Full LOLZ Repack (Limbo/Inside) ===="
    echo "SKIP: BLOCKED on PE32 Phase 4 (x87 FPU flags, TEST flags, and LAA 3GB not live)"
    exit 0
fi
