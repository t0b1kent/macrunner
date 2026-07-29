#!/usr/bin/env bash
# Idempotently deploy the Wine PE DLLs that apps load from a Wine prefix.
# Build/install writes dist; this script makes the runnable prefix match dist.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -f "$ROOT/config/env.sh" ]]; then
    # shellcheck disable=SC1091
    . "$ROOT/config/env.sh"
fi

DIST="${MACRUNNER_WINE_DIST_ARM64:-$ROOT/engine/wine/dist-pure-arm64}"
PREFIX="${MACRUNNER_NPP_PREFIX:-$ROOT/artifacts/phase-h/prefix-npp-x64-current}"
SYSTEM32_ARCH="${MACRUNNER_PREFIX_SYSTEM32_ARCH:-x86_64-windows}"

usage() {
    cat <<EOF
Usage: $0 [--dist PATH] [--prefix PATH] [--system32-arch ARCH]

Syncs PE module copies from Wine dist into the runnable Wine prefix:
  - all ARCH DLLs -> drive_c/windows/system32 (default: x86_64-windows)
  - all i386-windows DLLs -> drive_c/windows/syswow64
  - x86_64/i386/aarch64 comctl32_v6 -> matching WinSxS common-controls directories
EOF
}

while (($#)); do
    case "$1" in
        --dist=*) DIST="${1#--dist=}" ;;
        --dist) shift; DIST="${1:?missing --dist value}" ;;
        --prefix=*) PREFIX="${1#--prefix=}" ;;
        --prefix) shift; PREFIX="${1:?missing --prefix value}" ;;
        --system32-arch=*) SYSTEM32_ARCH="${1#--system32-arch=}" ;;
        --system32-arch) shift; SYSTEM32_ARCH="${1:?missing --system32-arch value}" ;;
        --help|-h) usage; exit 0 ;;
        *) echo "ERROR: unknown arg: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

WINE_LIB="$DIST/lib/wine"
DXMT_DIST="$ROOT/engine/graphics/dist/dxmt"
VKD3D_DIST="$ROOT/engine/graphics/dist/vkd3d"
WINDOWS_DIR="$PREFIX/drive_c/windows"
SYSTEM32="$WINDOWS_DIR/system32"
SYSWOW64="$WINDOWS_DIR/syswow64"
WINSXS="$WINDOWS_DIR/winsxs"
CORE_SYSTEM32_MODULES=(
    kernel32.dll
    user32.dll
    gdi32.dll
    shell32.dll
    comdlg32.dll
    kernelbase.dll
    ntdll.dll
    win32u.dll
    comctl32.dll
    explorerframe.dll
    winemac.drv
    # MacRunner 2026-07-29: every `.drv` MUST be listed here explicitly. copy_arch_set()'s bulk
    # loop globs `"$WINE_LIB/$arch"/*.dll`, so a driver is invisible to it purely because of the
    # extension — and the omission is silent, since nothing checks that a driver arrived.
    #
    # winecoreaudio.drv was missing, and that is why Hollow Knight has no sound: the prefix gets
    # dsound / mmdevapi / winmm / the whole xaudio2 row (they are .dll, so the glob takes them)
    # but never the CoreAudio backend they all sit on top of. With no backend mmdevapi enumerates
    # zero endpoints, and HK's FMOD reports it verbatim at +55.7 s of the menu-reaching run:
    # "FMOD failed to initialize any audio devices, running on emulated software output with no
    # sound." Same shape as the GraphicsDriver gap: the whole stack present, the one module that
    # talks to the host absent.
    winecoreaudio.drv
    msacm32.drv          # ACM codec driver — winmm's format conversion depends on it
    winspool.drv         # printing; harmless here, but it is the same silent-omission class
)
CORE_SYSTEM32_PROGRAMS=(
    wineboot.exe
    start.exe
    services.exe
    explorer.exe
    rundll32.exe
    control.exe
)
DXMT_MODULES=(
    d3d10core.dll
    d3d11.dll
    dxgi.dll
    winemetal.dll
)
VKD3D_MODULES=(
    d3d12.dll
    d3d12core.dll
)

[[ -d "$WINE_LIB/x86_64-windows" ]] || {
    echo "ERROR: missing dist x86_64-windows dir: $WINE_LIB/x86_64-windows" >&2
    exit 2
}
[[ -d "$WINE_LIB/i386-windows" ]] || {
    echo "ERROR: missing dist i386-windows dir: $WINE_LIB/i386-windows" >&2
    exit 2
}
[[ -d "$WINE_LIB/aarch64-windows" ]] || {
    echo "ERROR: missing dist aarch64-windows dir: $WINE_LIB/aarch64-windows" >&2
    exit 2
}
case "$SYSTEM32_ARCH" in
    x86_64-windows|aarch64-windows) ;;
    *) echo "ERROR: unsupported --system32-arch: $SYSTEM32_ARCH" >&2; exit 2 ;;
esac

rm -f "$SYSTEM32/ntdll.dll" "$SYSWOW64/ntdll.dll"

copy_dll() {
    local src="$1"
    local dst="$2"
    [[ -f "$src" ]] || { echo "ERROR: missing source DLL: $src" >&2; exit 2; }
    mkdir -p "$(dirname "$dst")"
    install -m 0644 "$src" "$dst"
    echo "synced ${dst#$ROOT/} <= ${src#$ROOT/}"
}

copy_arch_set() {
    local arch="$1"
    local target_dir="$2"
    local module src
    shopt -s nullglob
    for src in "$WINE_LIB/$arch"/*.dll; do
        # ntdll is the loader's architecture pivot in ARM64/x64 mixed runs.
        # Let Wine load the builtin ntdll from dist instead of staging a single
        # prefix copy that can hide the native/ARM64EC companion view.
        [[ "$(basename "$src")" == "ntdll.dll" ]] && continue
        copy_dll "$src" "$target_dir/$(basename "$src")"
    done
    shopt -u nullglob
    for module in "${CORE_SYSTEM32_MODULES[@]}"; do
        [[ "$module" == *.dll ]] && continue
        copy_dll "$WINE_LIB/$arch/$module" "$target_dir/$module"
    done
    for module in "${CORE_SYSTEM32_PROGRAMS[@]}"; do
        [[ -f "$WINE_LIB/$arch/$module" ]] || continue
        copy_dll "$WINE_LIB/$arch/$module" "$target_dir/$module"
    done
    if [[ -f "$WINE_LIB/$arch/comctl32_v6.dll" ]]; then
        copy_dll "$WINE_LIB/$arch/comctl32_v6.dll" "$target_dir/comctl32_v6.dll"
    fi
}

copy_arch_set "$SYSTEM32_ARCH" "$SYSTEM32"
copy_arch_set i386-windows "$SYSWOW64"

# Sync DXMT native PE frontends into the prefix so WINEDLLOVERRIDES can find them
# even when the loader does not search WINEDLLPATH early enough.  The x64
# winemetal.dll Unixlib handle is bridged to DXMT's aarch64-unix/winemetal.so by
# the HyperBridge NtQueryVirtualMemory(MemoryWineUnixFuncs) semantic.
for module in "${DXMT_MODULES[@]}"; do
    if [[ -f "$DXMT_DIST/$SYSTEM32_ARCH/$module" ]]; then
        copy_dll "$DXMT_DIST/$SYSTEM32_ARCH/$module" "$SYSTEM32/$module"
    fi
done

for module in "${VKD3D_MODULES[@]}"; do
    if [[ -f "$VKD3D_DIST/$SYSTEM32_ARCH/$module" ]]; then
        copy_dll "$VKD3D_DIST/$SYSTEM32_ARCH/$module" "$SYSTEM32/$module"
    fi
done

if [[ -f "$WINE_LIB/aarch64-windows/xtajit64.dll" ]]; then
    copy_dll "$WINE_LIB/aarch64-windows/xtajit64.dll" "$SYSTEM32/xtajit64.dll"
fi

# winemac.drv must be the NATIVE build in system32, same reasoning as xtajit64.dll above.
#
# Measured 2026-07-29 (HK-E2E-09, WINEDEBUG=-all,+loaddll).  In Hollow Knight's ARM64EC
# process every working wine builtin is loaded TWICE -- once as the x86_64 guest copy from
# C:\windows\system32\<dll> and once as a NATIVE aarch64 twin out of the dist (or, for
# dxgi/d3d11, out of laneA's per-run DXMT overlay).  37 modules get that twin, including
# win32u, user32, gdi32, dxgi, d3d11, winemetal.  winemac.drv gets ONLY the system32 x86_64
# copy and no twin:
#     0024: build_module Loaded L"c:\windows\system32\winemac.drv" at 0000087EF2E50000: builtin
# and no dllmain_attach ever follows for that wine pid, so macdrv_init never runs, the process
# keeps the re-entrancy placeholder as its user driver for life, nothing pumps AppKit, and
# injected keys measure macdrv_key_event = 0.
#
# The reason it is the odd one out is visible in user32/user_main.c:130 -- User32LoadDriver
# calls LdrLoadDll( L"c:\\windows\\system32", ... ), PINNING the search path, so the normal
# builtin-twin resolution that every other module goes through never happens for the display
# driver.  Putting the native build at that pinned path is the same shape of fix as the
# already-proven name-gated builtin-twin redirect used for the DXMT twins.
#
# Gated for a clean A/B (default OFF) because this lane's standing rule is that an offline
# proof is not a runtime proof -- MACRUNNER_WIN32U_PLACEHOLDER_REPAIR looked equally correct
# offline and regressed the guest.
if [[ -n "${MACRUNNER_PREFIX_WINEMAC_NATIVE:-}" && -f "$WINE_LIB/aarch64-windows/winemac.drv" ]]; then
    copy_dll "$WINE_LIB/aarch64-windows/winemac.drv" "$SYSTEM32/winemac.drv"
    echo "prefix_sync: winemac.drv <= aarch64-windows (native twin forced into system32)"
fi

# WOW64 backend modules required by 32-bit bottles:
#  - WOW64 CPU providers are shipped in different arches in this tree.
#  - xtajit is an aarch64 module for ARM64 host paths.
#  - wow64cpu remains x86_64-only (x64 thunk module) and is used as a helper.
if [[ -f "$WINE_LIB/aarch64-windows/xtajit.dll" ]]; then
    copy_dll "$WINE_LIB/aarch64-windows/xtajit.dll" "$SYSTEM32/xtajit.dll"
    copy_dll "$WINE_LIB/aarch64-windows/xtajit.dll" "$SYSWOW64/xtajit.dll"
fi
if [[ -f "$WINE_LIB/x86_64-windows/wow64cpu.dll" ]]; then
    copy_dll "$WINE_LIB/x86_64-windows/wow64cpu.dll" "$SYSTEM32/wow64cpu.dll"
    copy_dll "$WINE_LIB/x86_64-windows/wow64cpu.dll" "$SYSWOW64/wow64cpu.dll"
fi
if [[ -f "$WINE_LIB/$SYSTEM32_ARCH/wow64.dll" ]]; then
    copy_dll "$WINE_LIB/$SYSTEM32_ARCH/wow64.dll" "$SYSWOW64/wow64.dll"
fi
if [[ -f "$WINE_LIB/$SYSTEM32_ARCH/wow64win.dll" ]]; then
    copy_dll "$WINE_LIB/$SYSTEM32_ARCH/wow64win.dll" "$SYSWOW64/wow64win.dll"
fi

ensure_arm64_common_controls() {
    local template manifest_name arm64_manifest arm64_dir
    shopt -s nullglob
    for template in "$WINSXS"/manifests/{amd64,x86}_microsoft.windows.common-controls*_6.*.manifest; do
        manifest_name="$(basename "$template")"
        arm64_manifest="$WINSXS/manifests/arm64_${manifest_name#*_}"
        arm64_dir="$WINSXS/${arm64_manifest##*/}"
        arm64_dir="${arm64_dir%.manifest}"
        mkdir -p "$arm64_dir" "$(dirname "$arm64_manifest")"
        if [[ ! -f "$arm64_manifest" ]]; then
            sed -E 's/processorArchitecture="(amd64|x86|arm64)"/processorArchitecture="arm64"/' \
                "$template" >"$arm64_manifest"
            echo "created ${arm64_manifest#$ROOT/} <= ${template#$ROOT/}"
        fi
        return 0
    done
    return 1
}

if [[ -d "$WINSXS" ]]; then
    amd64_found=0
    for dir in "$WINSXS"/amd64_microsoft.windows.common-controls*_6.*; do
        [[ -d "$dir" ]] || continue
        amd64_found=1
        copy_dll "$WINE_LIB/x86_64-windows/comctl32_v6.dll" "$dir/comctl32.dll"
    done

    x86_found=0
    for dir in "$WINSXS"/x86_microsoft.windows.common-controls*_6.*; do
        [[ -d "$dir" ]] || continue
        x86_found=1
        copy_dll "$WINE_LIB/i386-windows/comctl32_v6.dll" "$dir/comctl32.dll"
    done

    arm64_found=0
    for dir in "$WINSXS"/arm64_microsoft.windows.common-controls*_6.*; do
        [[ -d "$dir" ]] || continue
        arm64_found=1
        copy_dll "$WINE_LIB/aarch64-windows/comctl32_v6.dll" "$dir/comctl32.dll"
    done
    if ((arm64_found == 0)); then
        if ensure_arm64_common_controls; then
            for dir in "$WINSXS"/arm64_microsoft.windows.common-controls*_6.*; do
                [[ -d "$dir" ]] || continue
                copy_dll "$WINE_LIB/aarch64-windows/comctl32_v6.dll" "$dir/comctl32.dll"
            done
        else
            echo "winsxs_comctl32_v6_sync=SKIP reason=no_common_controls_manifest_template"
        fi
    fi
else
    echo "winsxs_comctl32_v6_sync=SKIP reason=no_winsxs_dir"
fi

echo "prefix_sync=PASS prefix=$PREFIX dist=$DIST system32_arch=$SYSTEM32_ARCH"
