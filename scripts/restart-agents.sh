#!/usr/bin/env bash
# Restart helper for Codex CLI + Kimi with full perms, updated context, ccache.
# Run sections manually — this is a reference, not auto-executor.
set -euo pipefail
ROOT=/Volumes/MacOS/MacRunner
cd "$ROOT"
. config/env.sh   # CCACHE_DIR, paths, MACRUNNER_* anchors

echo "=== Pre-flight ==="
echo "CCACHE_DIR=$CCACHE_DIR"
ccache -s | head -3
export RUSTC_WRAPPER=sccache   # for Kimi's Rust cloud backend
echo "RUSTC_WRAPPER=$RUSTC_WRAPPER (sccache $(sccache --version 2>/dev/null | awk '{print $2}'))"

# Clean any leftover wine before fresh agent sessions
pkill -9 -f '[w]inetemp-|[w]ine-preloader|[w]inedbg|[n]otepad\+\+\.exe|[w]ineserver' 2>/dev/null || true

cat <<'CODEX_PROMPT'
=========================================================================
RESTART CODEX CLI — paste this as the launch prompt:
=========================================================================

codex --dangerously-bypass-approvals-and-sandbox --cd /Volumes/MacOS/MacRunner

Then first message:

Read /Volumes/MacOS/MacRunner/docs/CODEX-CONTEXT-RESTORE.md first, then
/Volumes/MacOS/MacRunner/AGENTS.md, then
/Volumes/MacOS/MacRunner/docs/CODEX-MEGA-DIRECTIVE-v2-batch-method.md.

Mission: Notepad++ x64 FULL FUNCTIONAL closure. NOT closed yet.
Method: BATCH-trace everything in one comprehensive run (all trace
categories, limits at 10M+), classify failures into families, batch-fix,
one rebuild, verify. Do NOT iterate one-symptom-at-a-time.

First action: ONE comprehensive trace run of Notepad++ with GetSysColor +
GDI + ImageList + font + opcode-fault traces all enabled, block limit 10M.
Hypothesis: GetSysColor returns black → black buttons/bands/scroll (one fix).
Collect ALL visual + opcode failures, classify, batch-fix families.

Source config/env.sh so builds use ccache (currently 0% — fix that).
Use Kimi's Visual Regression Lab (commit 9dc63c7) for automated artifact check.
Read reports/ai-configurator/CODEX-NEXT-TASK.md — Kimi routed tasks to you.

Close Kimi's engine-relevant findings (HB fault families from telemetry,
COM apartment INFINITE waits IF they affect clean exit).

NO KeePass until all 80 closure criteria green. Quality > speed.
=========================================================================
CODEX_PROMPT

cat <<'KIMI_PROMPT'
=========================================================================
RESTART KIMI — paste this as the launch prompt:
=========================================================================

(launch with full perms, same as current ollama claude kimi setup)

First message:

Read /Volumes/MacOS/MacRunner/docs/KIMI-CONTEXT-RESTORE.md first, then
/Volumes/MacOS/MacRunner/AGENTS.md, then
/Volumes/MacOS/MacRunner/docs/KIMI-MEGA-DIRECTIVE-v2-cloud-backend-pipeline.md.

Current primary: Cloud Backend + Unified Pipeline (Networking Δ.6 expanded).
Rust+axum+Postgres server that unifies your Configurator MVP, Visual
Regression Lab, AOT cache, telemetry, Networking client into shippable
backend. The commercial moat realized.

For Rust builds: export RUSTC_WRAPPER=sccache (installed).
Source config/env.sh for paths + ccache.

Phase Κ.0 first: architecture design + 3 ADRs (stack/storage/privacy).
NO server code until design done.

Standby (waits Codex Notepad++ functional): Audio E2E, AOT P3, Graphics e2e.
Opportunistic background: engine audit dim 05 (JIT/interp consistency).
=========================================================================
KIMI_PROMPT

echo ""
echo "Both agents: full perms, updated context via *-CONTEXT-RESTORE.md,"
echo "ccache (C) + sccache (Rust) configured. Obsidian binding via context"
echo "restore docs (survive memory wipe)."
