#!/bin/sh
# run19 — managed camera probe. Environment carried from run17's final-child.json
# by DENYLIST (113 vars), not by an allowlist regex: run18 died twice
# because an allowlist silently dropped WINEDLLPATH / WINESYSTEMDLLPATH /
# WINEDLLOVERRIDES. Values embedding run17's rundir are repointed at $RUNDIR.
set -eu
ROOT='/Users/timurtoby/Documents/MacRunner/Main/MacRunner'
STAMP=$(date +%H%M%S)
RUNDIR="$ROOT/reports/phase4-hollow-knight/laneA-INPUT-TEST-manual-language-BISECT-nostack-$STAMP"
mkdir -p "$RUNDIR"
# run28 orchestration fix (run27 data was nearly lost when the autoloop restart
# SIGTERMed the session holding the log): persist ALL output inside the run dir
# itself, so the evidence survives any session/loop death.
exec >> "$RUNDIR/run.log" 2>&1

# run17's launcher staged a per-run copy of the DXMT dist and pointed the Wine
# DLL search path at it. Reproduce that exactly, from the canonical dist whose
# 11 artifacts were verified byte-identical to run17's overlay.
OVERLAY="$RUNDIR/dxmt-builtin-overlay"
mkdir -p "$OVERLAY"
cp -R "$ROOT/engine/graphics/dist/dxmt/." "$OVERLAY/"
for a in aarch64-windows x86_64-windows aarch64-unix; do
  n=$(ls -1 "$OVERLAY/$a" 2>/dev/null | wc -l | tr -d ' ')
  echo "overlay $a: $n file(s)"
  [ "$n" -gt 0 ] || { echo "FATAL: overlay/$a is empty — this is what killed run18 attempt 1"; exit 1; }
done

export MACRUNNER_RUN_DIR="$RUNDIR"
export DXMT_LOG_PATH="$RUNDIR"
export MACRUNNER_DXMT_ROOT="$OVERLAY"
export MACRUNNER_FLIGHT_PATH="$RUNDIR/flight.jsonl"
export MACRUNNER_FLIGHT_RECORDER_FILE="$RUNDIR/flight.jsonl"
export MACRUNNER_FLIGHT_RECORDER_PATH="$RUNDIR/flight.jsonl"
# mr-run.sh prepends MACRUNNER_DXMT_ROOT to WINEDLLPATH, so supply the tail only;
# the result is byte-identical to run17's 5-entry path.
export WINEDLLPATH="$OVERLAY/x86_64-windows:$OVERLAY/x86_64-unix:/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/wine/dist-arm64ec-spike/lib/wine/x86_64-windows:/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/wine/dist-arm64ec-spike/lib/wine/x86_64-unix"

# ---- carried from run17 (denylist) ----
export AI_AGENT='claude-code_2-1-220_agent'
export ANTHROPIC_BASE_URL='https://api.anthropic.com'
export API_TIMEOUT_MS='900000'
export BAGGAGE='sentry-environment=production,sentry-release=Claude%401.24012.9,sentry-public_key=2f98127cbffe4740b1f767a2de77d23b,sentry-trace_id=7308a6b50ed24991b6d168b7bd2b66df,sentry-org_id=1158394'
export BISON='/opt/homebrew/opt/bison/bin/bison'
export CC='ccache /usr/bin/clang'
export CCACHE_BASEDIR='/Users/timurtoby/Documents/MacRunner/Main/MacRunner'
export CCACHE_DIR='/Users/timurtoby/Documents/MacRunner/Main/MacRunner/artifacts/ccache'
export CLAUDECODE='1'
export CLAUDE_AGENT_SDK_VERSION='0.3.219'
export CLAUDE_CODE_CHILD_SESSION='1'
export CLAUDE_CODE_DISABLE_CRON=''
export CLAUDE_CODE_EAGER_FLUSH='1'
export CLAUDE_CODE_EMIT_TOOL_USE_SUMMARIES='false'
export CLAUDE_CODE_ENABLE_ASK_USER_QUESTION_TOOL='true'
export CLAUDE_CODE_ENTRYPOINT='claude-desktop'
export CLAUDE_CODE_EXECPATH='/opt/homebrew/lib/node_modules/@anthropic-ai/claude-code/bin/claude.exe'
export CLAUDE_CODE_HOST_SESSION_ID='local_9aa74ab6-2811-4d7b-bdd3-e907325b7c6c'
export CLAUDE_CODE_OAUTH_SCOPES='user:inference user:file_upload user:profile user:sessions:claude_code'
export CLAUDE_CODE_SDK_HAS_HOST_AUTH_REFRESH='1'
export CLAUDE_CODE_SDK_HAS_OAUTH_REFRESH='1'
export CLAUDE_EFFORT='xhigh'
export CLAUDE_PREVIEW_CLASSIFIER_FLOOR='1'
export COMMAND_MODE='unix2003'
export COREPACK_ENABLE_AUTO_PIN='0'
export CXX='ccache /usr/bin/clang++'
export DISABLE_AUTOUPDATER='1'
export DISABLE_MICROCOMPACT='1'
export DXMT_LOG_LEVEL='info'
export DYLD_FALLBACK_LIBRARY_PATH='/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/wine/dist-arm64ec-spike/lib/wine/aarch64-unix:/opt/homebrew/lib:/opt/homebrew/opt/freetype/lib:/usr/local/lib:/usr/lib'
export DYLD_LIBRARY_PATH='/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/wine/dist-arm64ec-spike/lib/wine/aarch64-unix:/opt/homebrew/lib:/opt/homebrew/opt/freetype/lib'
export FLEX='/opt/homebrew/opt/flex/bin/flex'
export GIT_EDITOR='true'
export HOME='/Users/timurtoby'
export LC_CTYPE='C.UTF-8'
export LEX='/opt/homebrew/opt/flex/bin/flex'
export LOGNAME='timurtoby'
export MACRUNNER_ARTIFACTS_ROOT='/Users/timurtoby/Documents/MacRunner/Main/MacRunner/artifacts'
export MACRUNNER_BOTTLES_ROOT='/Users/timurtoby/Documents/MacRunner/Main/MacRunner/bottles'
export MACRUNNER_CCACHE_DIR='/Users/timurtoby/Documents/MacRunner/Main/MacRunner/artifacts/ccache'
export MACRUNNER_DXMT_FRAME_DUMP='1'
export MACRUNNER_FLIGHT_RECORDER='1'
export MACRUNNER_GRAPHICS_BACKEND='dxmt'
export MACRUNNER_HB_BACKEND='jit'
export MACRUNNER_HB_DIRECT_MEM='1'
# ---- run20 arm: the DIRECT-MEMORY LOWERING FAMILY, off ----
# Deliberately two knobs, not one. direct_user_xmm_mem_allowed() consults only
# direct_mem_codegen_arch_enabled() (arch check) AND MACRUNNER_HB_JIT_DIRECT_XMM_MEM
# (hb_arm64_codegen.c:505, DEFAULT 1) -- it never consults JIT_DIRECT_MEM. So
# flipping JIT_DIRECT_MEM alone would leave the 128-bit path fully enabled and
# the arm would prove nothing. One HYPOTHESIS (direct-memory lowering), two knobs;
# if m33 flips, the next run bisects which. At ~50 min per arm, broad-then-bisect
# is the right order.
export MACRUNNER_HB_JIT_DIRECT_MEM='1'   # FAST-JIT: was 0 (lane's observability profile) -> ON
export MACRUNNER_HB_JIT_DIRECT_XMM_MEM='1'   # FAST-JIT: was 0 (lane's observability profile) -> ON
export MACRUNNER_HB_JIT_DIRECT_SCALAR_MEM='1'   # FAST-JIT: was 0 (lane's observability profile) -> ON
export MACRUNNER_HB_JIT_DIRECT_SCALAR_SCAN='1'
export MACRUNNER_HB_JIT_DIRECT_STACK='0'   # BISECT: this one OFF   # FAST-JIT: was 0 (lane's observability profile) -> ON
export MACRUNNER_HB_LANGUAGE_FLOW_OBSERVER='1'
export MACRUNNER_HB_LANGUAGE_ONESHOT_SEQUENCE='1'
export MACRUNNER_HB_PRESENT_SURFACE_READBACK='1'
export MACRUNNER_HB_PRESENT_SURFACE_READBACK_ORDINAL='200'
export MACRUNNER_HB_SINGLE_LOOKUP='1'
export MACRUNNER_HB_TRACE_D3D_BOUNDARY='1'
export MACRUNNER_HB_TRACE_DXGI_SWAPCHAIN='1'
export MACRUNNER_HB_TRACE_TRANSLATION_CACHE='1'
export MACRUNNER_HB_TRANSLATION_CACHE='1'
export MACRUNNER_HB_TRANSLATION_CACHE_ROOT='/Users/timurtoby/Documents/MacRunner/Main/MacRunner/artifacts/hb-translation-cache/ntdll-5385710288b50361'
export MACRUNNER_HB_X64_LOADER='1'
export MACRUNNER_HOMEBREW_LIBRARY_PATHS='/opt/homebrew/lib:/opt/homebrew/opt/freetype/lib'
export MACRUNNER_HOMEBREW_PREFIX='/opt/homebrew'
export MACRUNNER_LLVM_MINGW_BIN='/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/bin'
export MACRUNNER_MR_RUN_ACTXPRXY='0'
export MACRUNNER_MR_RUN_CRT_CASE_FUSION='0'
export MACRUNNER_MR_RUN_PREFIX_TEMPLATE='/Users/timurtoby/Documents/MacRunner/Main/MacRunner/artifacts/hk-prefix-template-NOLANG-inputtest'
export MACRUNNER_MR_RUN_SKIP_WINEBOOT='1'
export MACRUNNER_MR_RUN_START_SERVICES='1'
export MACRUNNER_MR_RUN_WWISE_OBSERVER='0'
export MACRUNNER_NPP_AFTER_LAUNCH_READY_SECS='240'
export MACRUNNER_NPP_X64_APP='/Users/timurtoby/Documents/MacRunner/Main/MacRunner/artifacts/phase-h/npp-x64/notepad++.exe'
export MACRUNNER_NPP_X64_DIR='/Users/timurtoby/Documents/MacRunner/Main/MacRunner/artifacts/phase-h/npp-x64'
export MACRUNNER_PREFIX_SYSTEM32_ARCH='x86_64-windows'
export MACRUNNER_REPORTS_ROOT='/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports'
export MACRUNNER_ROOT='/Users/timurtoby/Documents/MacRunner/Main/MacRunner'
export MACRUNNER_WINEBOOT_WAIT_SECS='180'
export MACRUNNER_WINESERVER_WAIT_SECS='120'
export MACRUNNER_WINE_BIN='/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/wine/dist/bin/wine'
export MACRUNNER_WINE_BUILD_ARM64='/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/wine/build-pure-arm64'
export MACRUNNER_WINE_CCACHE_PREFIX='ccache '
export MACRUNNER_WINE_DIST='/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/wine/dist'
export MACRUNNER_WINE_DIST_ARM64='/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/wine/dist-pure-arm64'
export MCP_CONNECTION_NONBLOCKING='true'
export MONO_ENV_OPTIONS='--profile=hk_language'
export MallocNanoZone='0'
export NODE_USE_SYSTEM_CA='1'
export NoDefaultCurrentDirectoryInExePath='1'
export OSLogRateLimit='64'
export PATH='/Users/timurtoby/.kimi-code/bin:/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/bin:/opt/homebrew/opt/flex/bin:/opt/homebrew/opt/bison/bin:/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/bin:/Users/timurtoby/.kimi-code/bin:/Users/timurtoby/.bun/bin:/Users/timurtoby/.bun/bin:/usr/local/opt/ruby/bin:/Library/Frameworks/Python.framework/Versions/3.12/bin:/Library/Frameworks/Python.framework/Versions/3.13/bin:/Library/Frameworks/Python.framework/Versions/3.8/bin:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/System/Cryptexes/App/usr/bin:/usr/bin:/bin:/usr/sbin:/sbin:/var/run/com.apple.security.cryptexd/codex.system/bootstrap/usr/local/bin:/var/run/com.apple.security.cryptexd/codex.system/bootstrap/usr/bin:/var/run/com.apple.security.cryptexd/codex.system/bootstrap/usr/appleinternal/bin:/pkg/env/global/bin:/Library/Apple/usr/bin:/Users/timurtoby/.cargo/bin:/Users/timurtoby/spoofdpi:/Users/timurtoby/.cache/lm-studio/bin:/Users/timurtoby/bin:/Users/timurtoby/.pyenv/shims:/Users/timurtoby/.claude/plugins/cache/claude-plugins-official/clangd-lsp/1.0.0/bin:/Users/timurtoby/Library/Application Support/Claude/local-agent-mode-sessions/skills-plugin/0ed46f59-caa6-4e64-bb93-29f5b67b156f/1c15064b-5e4d-4271-a5a3-c9d22575c5ac/bin:/Users/timurtoby/.claude/plugins/cache/claude-plugins-official/clangd-lsp/1.0.0/bin:/Users/timurtoby/.claude/plugins/cache/claude-plugins-official/clangd-lsp/1.0.0/bin'
export SHELL='/bin/zsh'
export SSH_AUTH_SOCK='/var/run/com.apple.launchd.hFZhVXYJg3/Listeners'
export TMPDIR='/var/folders/48/2f66hmz52n5738l03xhspcyw0000gn/T/'
export USER='timurtoby'
export USE_LOCAL_OAUTH=''
export USE_STAGING_OAUTH=''
export WINEDEBUG='-all'
export WINEDLLOVERRIDES='mono-profiler-hk_language=n;d3d9=n,b;d3d11,dxgi,d3d10core,winemetal=n,b'
export WINELOADERNOEXEC='1'
export WINEMSYNC='1'
export WINESYSTEMDLLPATH="$RUNDIR/dxmt-builtin-overlay/x86_64-windows"   # repointed at $RUNDIR
export XPC_FLAGS='0x0'
export XPC_SERVICE_NAME='0'
export YACC='/opt/homebrew/opt/bison/bin/bison -y'
export __CFBundleIdentifier='com.anthropic.claudefordesktop'
export __CF_USER_TEXT_ENCODING='0x1F5:0x0:0x0'
export aarch64_CC='ccache /Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/bin/aarch64-w64-mingw32-gcc'
export aarch64_CXX='ccache /Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/bin/aarch64-w64-mingw32-g++'
export arm64ec_CC='ccache /Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/bin/arm64ec-w64-mingw32-gcc'
export arm64ec_CXX='ccache /Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/bin/arm64ec-w64-mingw32-g++'
export i386_CC='ccache /Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/bin/i686-w64-mingw32-gcc'
export i386_CXX='ccache /Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/bin/i686-w64-mingw32-g++'
export x86_64_CC='ccache /Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/bin/x86_64-w64-mingw32-gcc'
export x86_64_CXX='ccache /Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/bin/x86_64-w64-mingw32-g++'

# ---- run19 additions ----
export MACRUNNER_HB_CAMERA_PROBE='1'
export MACRUNNER_HB_CAMERA_PROBE_MAX='16'
export MACRUNNER_HB_CAMERA_PROBE_STRIDE='300'
export MACRUNNER_HB_LANGUAGE_FLOW_OBSERVER_MAX='1024'

# ---- run24: press Start Game ----
# The acceptance checklist (MakeMenuLean/Opening_Sequence/Levels are ready) is only
# reachable from <RunStartNewGame>d__359 / <RunContinueGame>d__361, both PLAYER-initiated.
# run20 rendered the menu correctly and waited for a keypress nothing ever sends.
# env_enabled() is VALUE-based (value[0] && value[0] != '0'), so '1' enables.
export MACRUNNER_HB_START_GAME_ACTUATOR='0'   # run30: OFF on purpose — the point is whether the game skips the menu WITHOUT help
export MACRUNNER_HB_START_GAME_ACTUATOR_MAX='0'

# run25's ONLY new variable. Default OFF in the DLL and gated on the actuator, so
# with it unset the binary is byte-identical behaviour to run24's.
# run30: disabled (clean run) — MACRUNNER_HB_NEWOBJ_PROBE


# ---- run27: alloc-return-value oracle (mono rva 0x1cfd70, rcx == allocated obj) ----
# Default OFF in the DLL, so with it unset this binary behaves as run26's.
# run30: disabled (clean run) — MACRUNNER_HB_MONO_ALLOC_ORACLE

# ---- run29: which tail arm executes (NULL 0x1cfd8f vs OK 0x1cfdaf)? ----
# Env-only discriminator; the tail blocks are never disk-cached (helper-call blobs
# are declined by native_blob_prepare_cache_store) so they re-lift every run and the
# translation-time trace cannot miss them. Entry block = liveness control.
# run30: disabled (clean run) — MACRUNNER_HB_TRACE_JIT_BLOCKS
# run30: disabled (clean run) — MACRUNNER_HB_TRACE_JIT_GUEST_ADDR
# run30: disabled (clean run) — MACRUNNER_HB_TRACE_JIT_GUEST_RANGE_START
# run30: disabled (clean run) — MACRUNNER_HB_TRACE_JIT_GUEST_RANGE_END
# run30: disabled (clean run) — MACRUNNER_HB_TRACE_JIT_BLOCK_BUDGET

echo "BISECT: all direct-memory JIT paths ON except DIRECT_STACK (rbp is the frame pointer): oracle template MINUS the two language keys -> save snapshot satisfied AND menu appears"
echo "WINEDLLOVERRIDES=$WINEDLLOVERRIDES"
echo "WINESYSTEMDLLPATH=${WINESYSTEMDLLPATH:-<unset>}"
exec "$ROOT/scripts/mr-run.sh" \
  "$ROOT/engine/wine/dist-arm64ec-spike" \
  '/Users/timurtoby/Documents/MacRunner/Main/MacRunner/../game-hollow.knight-(89718)/extracted-hollow-knight-1.5.12620/Hollow Knight.exe' \
  2400