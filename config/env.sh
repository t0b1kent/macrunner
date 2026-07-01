# MacRunner — единая конфигурация путей для всех скриптов
# Source-ить в начале каждого скрипта: . "$PROJECT_ROOT/config/env.sh"

# Корень проекта.  Default to the current worktree when sourced from its root;
# build scripts set MACRUNNER_ROOT explicitly before sourcing this file.
: "${MACRUNNER_ROOT:=$([ -f "$PWD/config/env.sh" ] && pwd || printf '%s' /Users/timurtoby/Documents/MacRunner/Main/MacRunner)}"
export MACRUNNER_ROOT

# Все Wine bottles живут рядом с активным движком.
# Раньше: $HOME/Library/Application Support/MacRunner/bottles/ (внутренний SSD, износ)
# Сейчас: $MACRUNNER_ROOT/bottles/ в canonical workspace. Если нужно вынести bottles,
# переопредели MACRUNNER_BOTTLES_ROOT явно перед source config/env.sh.
: "${MACRUNNER_BOTTLES_ROOT:=$MACRUNNER_ROOT/bottles}"
export MACRUNNER_BOTTLES_ROOT
mkdir -p "$MACRUNNER_BOTTLES_ROOT" 2>/dev/null || true

# Reports / artifacts
: "${MACRUNNER_REPORTS_ROOT:=$MACRUNNER_ROOT/reports}"
: "${MACRUNNER_ARTIFACTS_ROOT:=$MACRUNNER_ROOT/artifacts}"
export MACRUNNER_REPORTS_ROOT MACRUNNER_ARTIFACTS_ROOT

# Compiler cache is mandatory for local rebuild velocity. Disable only with
# MACRUNNER_USE_CCACHE=0 when debugging compiler/cache behavior.
: "${MACRUNNER_CCACHE_DIR:=$MACRUNNER_ARTIFACTS_ROOT/ccache}"
export MACRUNNER_CCACHE_DIR
if command -v ccache >/dev/null 2>&1; then
    : "${CCACHE_DIR:=$MACRUNNER_CCACHE_DIR}"
    : "${CCACHE_BASEDIR:=$MACRUNNER_ROOT}"
    export CCACHE_DIR CCACHE_BASEDIR
    mkdir -p "$CCACHE_DIR" 2>/dev/null || true
fi

# Pinned Wine build toolchain.
#
# Keep this in shared env, not only in build scripts: targeted Wine relinks are
# often run directly from the source/build directory.  Those relinks must not
# fall back to Apple /usr/bin/bison (2.3) or non-pinned mingw tools.  Use
# LLVM-mingw's gcc-compatible target drivers for PE builds; the i386 wrapper
# path avoids comctl32/treeview.o EH-label assembler failures seen with direct
# *-clang clean-build invocations.
: "${MACRUNNER_LLVM_MINGW_BIN:=$MACRUNNER_ROOT/engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/bin}"
: "${MACRUNNER_HOMEBREW_PREFIX:=/opt/homebrew}"

macrunner_prepend_path()
{
    [ -n "$1" ] || return 0
    [ -d "$1" ] || return 0
    case ":$PATH:" in
        *":$1:"*) ;;
        *) PATH="$1:$PATH" ;;
    esac
}

macrunner_prepend_path "$MACRUNNER_LLVM_MINGW_BIN"
macrunner_prepend_path "$MACRUNNER_HOMEBREW_PREFIX/opt/bison/bin"
macrunner_prepend_path "$MACRUNNER_HOMEBREW_PREFIX/opt/flex/bin"
macrunner_prepend_path "$MACRUNNER_HOMEBREW_PREFIX/bin"
macrunner_prepend_path "$MACRUNNER_HOMEBREW_PREFIX/sbin"
export PATH MACRUNNER_LLVM_MINGW_BIN MACRUNNER_HOMEBREW_PREFIX

if [ -x "$MACRUNNER_HOMEBREW_PREFIX/opt/bison/bin/bison" ]; then
    : "${BISON:=$MACRUNNER_HOMEBREW_PREFIX/opt/bison/bin/bison}"
    : "${YACC:=$BISON -y}"
    export BISON YACC
fi
if [ -x "$MACRUNNER_HOMEBREW_PREFIX/opt/flex/bin/flex" ]; then
    : "${FLEX:=$MACRUNNER_HOMEBREW_PREFIX/opt/flex/bin/flex}"
    : "${LEX:=$FLEX}"
    export FLEX LEX
fi

if command -v ccache >/dev/null 2>&1 && [ "${MACRUNNER_USE_CCACHE:-1}" != "0" ]; then
    MACRUNNER_WINE_CCACHE_PREFIX="ccache "
else
    MACRUNNER_WINE_CCACHE_PREFIX=""
fi
export MACRUNNER_WINE_CCACHE_PREFIX

: "${CC:=${MACRUNNER_WINE_CCACHE_PREFIX}/usr/bin/clang}"
: "${CXX:=${MACRUNNER_WINE_CCACHE_PREFIX}/usr/bin/clang++}"
: "${aarch64_CC:=${MACRUNNER_WINE_CCACHE_PREFIX}${MACRUNNER_LLVM_MINGW_BIN}/aarch64-w64-mingw32-gcc}"
: "${aarch64_CXX:=${MACRUNNER_WINE_CCACHE_PREFIX}${MACRUNNER_LLVM_MINGW_BIN}/aarch64-w64-mingw32-g++}"
: "${arm64ec_CC:=${MACRUNNER_WINE_CCACHE_PREFIX}${MACRUNNER_LLVM_MINGW_BIN}/arm64ec-w64-mingw32-gcc}"
: "${arm64ec_CXX:=${MACRUNNER_WINE_CCACHE_PREFIX}${MACRUNNER_LLVM_MINGW_BIN}/arm64ec-w64-mingw32-g++}"
: "${x86_64_CC:=${MACRUNNER_WINE_CCACHE_PREFIX}${MACRUNNER_LLVM_MINGW_BIN}/x86_64-w64-mingw32-gcc}"
: "${x86_64_CXX:=${MACRUNNER_WINE_CCACHE_PREFIX}${MACRUNNER_LLVM_MINGW_BIN}/x86_64-w64-mingw32-g++}"
: "${i386_CC:=${MACRUNNER_WINE_CCACHE_PREFIX}${MACRUNNER_LLVM_MINGW_BIN}/i686-w64-mingw32-gcc}"
: "${i386_CXX:=${MACRUNNER_WINE_CCACHE_PREFIX}${MACRUNNER_LLVM_MINGW_BIN}/i686-w64-mingw32-g++}"
export CC CXX aarch64_CC aarch64_CXX arm64ec_CC arm64ec_CXX x86_64_CC x86_64_CXX i386_CC i386_CXX

# Wine dist
: "${MACRUNNER_WINE_DIST:=$MACRUNNER_ROOT/engine/wine/dist}"
: "${MACRUNNER_WINE_BIN:=$MACRUNNER_WINE_DIST/bin/wine}"
export MACRUNNER_WINE_DIST MACRUNNER_WINE_BIN

# Pure-arm64 Wine dist (HyperBridge x64 lane) — for cross-arch x64 PE launches.
: "${MACRUNNER_WINE_DIST_ARM64:=$MACRUNNER_ROOT/engine/wine/dist-pure-arm64}"
: "${MACRUNNER_WINE_BUILD_ARM64:=$MACRUNNER_ROOT/engine/wine/build-pure-arm64}"
export MACRUNNER_WINE_DIST_ARM64 MACRUNNER_WINE_BUILD_ARM64

# Phase H test-app anchors. Canonical paths — see AGENTS.md, do not invent alternatives.
: "${MACRUNNER_NPP_X64_DIR:=$MACRUNNER_ARTIFACTS_ROOT/phase-h/npp-x64}"
: "${MACRUNNER_NPP_X64_APP:=$MACRUNNER_NPP_X64_DIR/notepad++.exe}"
export MACRUNNER_NPP_X64_DIR MACRUNNER_NPP_X64_APP

# Системные библиотеки Homebrew (libvulkan, MoltenVK, FreeType/font backend).
#
# Wine loads some optional host libraries with dlopen("libfoo.dylib") instead
# of an absolute path.  Apple Silicon Homebrew lives under /opt/homebrew, which
# is not in dyld's default fallback search path; without these entries GDI text
# measurement can silently fail because win32u cannot find libfreetype.
: "${MACRUNNER_HOMEBREW_LIBRARY_PATHS:=/opt/homebrew/lib:/opt/homebrew/opt/freetype/lib}"
: "${DYLD_LIBRARY_PATH:=$MACRUNNER_HOMEBREW_LIBRARY_PATHS}"
: "${DYLD_FALLBACK_LIBRARY_PATH:=$MACRUNNER_HOMEBREW_LIBRARY_PATHS:/usr/local/lib:/usr/lib}"
export MACRUNNER_HOMEBREW_LIBRARY_PATHS DYLD_LIBRARY_PATH DYLD_FALLBACK_LIBRARY_PATH

# Wine launch timeouts — щедрые, но ОГРАНИЧЕННЫЕ (не бесконечные).
# Зачем: при тяжёлой трассировке (SIMD-дампы, opcode-fault, block limit 10M) Wine
# работает в разы медленнее, и дефолтные таймауты (ready=30s, wineserver=20s,
# wineboot=60s) ловят медленные прогоны как "timeout" — Codex теряет на этом время.
# Здесь заданы раз и навсегда для ВСЕХ скриптов (run-notepad-x64.sh и т.д.).
# НЕ бесконечные намеренно: настоящий deadlock должен всё же падать за пару минут,
# а не висеть вечно. Переопределяемы (можно поднять для совсем тяжёлой трассы).
: "${MACRUNNER_NPP_AFTER_LAUNCH_READY_SECS:=240}"   # окно/готовность app (было 30)
: "${MACRUNNER_WINESERVER_WAIT_SECS:=120}"          # ожидание wineserver (было 20)
: "${MACRUNNER_WINEBOOT_WAIT_SECS:=180}"            # wineboot init/update (было 60)
export MACRUNNER_NPP_AFTER_LAUNCH_READY_SECS MACRUNNER_WINESERVER_WAIT_SECS MACRUNNER_WINEBOOT_WAIT_SECS

# Удобные shortcuts для скриптов
macrunner_bottle() {
    # Usage: macrunner_bottle <name> → echoes the path, creates dir
    local name="${1:-default}"
    local prefix="$MACRUNNER_BOTTLES_ROOT/$name"
    mkdir -p "$prefix"
    echo "$prefix"
}
