#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="${PROJECT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
source "$PROJECT_ROOT/config/env.sh"
source "$PROJECT_ROOT/engine/graphics/build-support/common.sh"

ARCH="${1:-aarch64}"
BUILD_DIR="$GRAPHICS_BUILD/dxmt-${ARCH}-tests"
CROSS_FILE="$GRAPHICS_BUILD/cross/dxmt-${ARCH}-tests.ini"
NATIVE_LLVM_PATH="${NATIVE_LLVM_PATH:-/opt/homebrew/opt/llvm@15}"
DXMT_SMOKE_BIND_MODE="${DXMT_SMOKE_BIND_MODE:-mixed}"
WINE_DIST="$PROJECT_ROOT/engine/wine/dist"
WINE="$WINE_DIST/bin/wine"
WINESERVER="$WINE_DIST/bin/wineserver"
PREFIX="$PROJECT_ROOT/artifacts/dxmt-smoke-prefix"
PREFIX_SYSTEM32="$PREFIX/drive_c/windows/system32"
APP_DIR="$PREFIX/drive_c/dxmt-smoke"
OVERLAY_DIR="$PREFIX/dxmt-builtin-overlay"
LOG_DIR="$PROJECT_ROOT/artifacts/dxmt-smoke-logs"
LOG="$LOG_DIR/dx11-headless-${ARCH}.log"
SMOKE_TIMEOUT_SECONDS="${SMOKE_TIMEOUT_SECONDS:-120}"
SMOKE_REPEAT_COUNT="${SMOKE_REPEAT_COUNT:-1}"
SMOKE_STABILITY_FRAMES="${SMOKE_STABILITY_FRAMES:-180}"
SMOKE_STABILITY_MIN_MS="${SMOKE_STABILITY_MIN_MS:-0}"
SMOKE_VISIBLE_WINDOW="${SMOKE_VISIBLE_WINDOW:-0}"
POSTPROCESS_WINEMETAL_ONLY=false

case "$DXMT_SMOKE_BIND_MODE" in
  native)
    WINE_BUILTIN_DLL="${WINE_BUILTIN_DLL:-false}"
    DXMT_SMOKE_DLL_OVERRIDES="${DXMT_SMOKE_DLL_OVERRIDES:-d3d11,dxgi,winemetal=n}"
    ;;
  mixed)
    WINE_BUILTIN_DLL="${WINE_BUILTIN_DLL:-false}"
    DXMT_SMOKE_DLL_OVERRIDES="${DXMT_SMOKE_DLL_OVERRIDES:-d3d11,dxgi=n;winemetal=b,n}"
    POSTPROCESS_WINEMETAL_ONLY=true
    ;;
  builtin)
    WINE_BUILTIN_DLL="${WINE_BUILTIN_DLL:-true}"
    DXMT_SMOKE_DLL_OVERRIDES="${DXMT_SMOKE_DLL_OVERRIDES:-d3d11,dxgi,winemetal=b,n}"
    ;;
  *)
    echo "unsupported DXMT smoke bind mode: $DXMT_SMOKE_BIND_MODE" >&2
    exit 24
    ;;
esac

case "$ARCH" in
  aarch64|arm64) MACHINE_DIR="aarch64-windows"; UNIX_DIR="aarch64-unix" ;;
  x86_64|amd64) MACHINE_DIR="x86_64-windows"; UNIX_DIR="x86_64-unix" ;;
  i386|x86) MACHINE_DIR="i386-windows"; UNIX_DIR="i386-unix" ;;
  *) echo "unsupported DXMT smoke arch: $ARCH" >&2; exit 21 ;;
esac

OVERLAY_MACHINE_DIR="$OVERLAY_DIR/$MACHINE_DIR"
OVERLAY_UNIX_DIR="$OVERLAY_DIR/$UNIX_DIR"
WINE_MACHINE_DIR="$WINE_DIST/lib/wine/$MACHINE_DIR"
WINE_UNIX_DIR="$WINE_DIST/lib/wine/$UNIX_DIR"

ensure_build_tools
ensure_llvm_mingw
write_windows_cross_file "$ARCH" "$CROSS_FILE"

if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
  "${MESON[@]}" setup "$BUILD_DIR" "$PROJECT_ROOT/engine/dxmt" \
    --cross-file "$CROSS_FILE" \
    -Denable_tests=true \
    -Dwine_builtin_dll="$WINE_BUILTIN_DLL" \
    -Dnative_llvm_path="$NATIVE_LLVM_PATH" \
    -Dwine_install_path="$WINE_DIST"
else
  "${MESON[@]}" configure "$BUILD_DIR" \
    -Denable_tests=true \
    -Dwine_builtin_dll="$WINE_BUILTIN_DLL" \
    -Dnative_llvm_path="$NATIVE_LLVM_PATH" \
    -Dwine_install_path="$WINE_DIST"
fi

ninja -C "$BUILD_DIR" tests/dx11/dx11_headless_smoke.exe src/winemetal/unix/winemetal.so

if [[ "$WINE_BUILTIN_DLL" == "true" ]]; then
  ninja -C "$BUILD_DIR" src/d3d11/d3d11.dll.postproc src/dxgi/dxgi.dll.postproc src/winemetal/winemetal.dll.postproc
else
  rm -f "$BUILD_DIR/src/d3d11/d3d11.dll" "$BUILD_DIR/src/dxgi/dxgi.dll" "$BUILD_DIR/src/winemetal/winemetal.dll"
  ninja -C "$BUILD_DIR" src/d3d11/d3d11.dll src/dxgi/dxgi.dll src/winemetal/winemetal.dll
  if [[ "$POSTPROCESS_WINEMETAL_ONLY" == "true" ]]; then
    "$WINE_DIST/bin/winebuild" --builtin "$BUILD_DIR/src/winemetal/winemetal.dll"
  fi
fi

EXE="$BUILD_DIR/tests/dx11/dx11_headless_smoke.exe"
D3D11_DLL="$BUILD_DIR/src/d3d11/d3d11.dll"
DXGI_DLL="$BUILD_DIR/src/dxgi/dxgi.dll"
WINEMETAL_DLL="$BUILD_DIR/src/winemetal/winemetal.dll"
WINEMETAL_SO="$BUILD_DIR/src/winemetal/unix/winemetal.so"

if [[ ! -x "$WINE" || ! -x "$WINESERVER" ]]; then
  echo "missing Wine runtime under $WINE_DIST" >&2
  exit 20
fi
if [[ ! -d "$WINE_MACHINE_DIR" ]]; then
  echo "missing Wine PE builtin dir: $WINE_MACHINE_DIR" >&2
  exit 22
fi
if [[ ! -d "$WINE_UNIX_DIR" ]]; then
  echo "missing Wine Unix builtin dir: $WINE_UNIX_DIR" >&2
  exit 23
fi

mkdir -p "$PREFIX" "$LOG_DIR"
WINEPREFIX="$PREFIX" "$WINESERVER" -k >/dev/null 2>&1 || true

mkdir -p "$PREFIX_SYSTEM32" "$APP_DIR" "$OVERLAY_MACHINE_DIR" "$OVERLAY_UNIX_DIR"
find "$OVERLAY_MACHINE_DIR" -mindepth 1 -maxdepth 1 -exec rm -f {} +
find "$OVERLAY_UNIX_DIR" -mindepth 1 -maxdepth 1 -exec rm -f {} +
find "$WINE_MACHINE_DIR" -mindepth 1 -maxdepth 1 -exec sh -c 'ln -s "$1" "$2/$(basename "$1")"' sh {} "$OVERLAY_MACHINE_DIR" \;
find "$WINE_UNIX_DIR" -mindepth 1 -maxdepth 1 -exec sh -c 'ln -s "$1" "$2/$(basename "$1")"' sh {} "$OVERLAY_UNIX_DIR" \;
rm -f "$OVERLAY_MACHINE_DIR/d3d11.dll" "$OVERLAY_MACHINE_DIR/dxgi.dll" "$OVERLAY_MACHINE_DIR/winemetal.dll"
rm -f "$OVERLAY_UNIX_DIR/winemetal.so" \
  "$OVERLAY_MACHINE_DIR/winemetal.so" "$OVERLAY_MACHINE_DIR/winemetal.dll.so" \
  "$APP_DIR/winemetal.so" "$APP_DIR/winemetal.dll.so" \
  "$PREFIX_SYSTEM32/winemetal.so" "$PREFIX_SYSTEM32/winemetal.dll.so"
mkdir -p "$PREFIX_SYSTEM32" "$APP_DIR" "$OVERLAY_MACHINE_DIR" "$OVERLAY_UNIX_DIR"
cp -f "$D3D11_DLL" "$PREFIX_SYSTEM32/d3d11.dll"
cp -f "$DXGI_DLL" "$PREFIX_SYSTEM32/dxgi.dll"
cp -f "$WINEMETAL_DLL" "$PREFIX_SYSTEM32/winemetal.dll"
cp -f "$EXE" "$APP_DIR/dx11_headless_smoke.exe"
cp -f "$D3D11_DLL" "$APP_DIR/d3d11.dll"
cp -f "$DXGI_DLL" "$APP_DIR/dxgi.dll"
cp -f "$WINEMETAL_DLL" "$APP_DIR/winemetal.dll"
cp -f "$D3D11_DLL" "$OVERLAY_MACHINE_DIR/d3d11.dll"
cp -f "$DXGI_DLL" "$OVERLAY_MACHINE_DIR/dxgi.dll"
cp -f "$WINEMETAL_DLL" "$OVERLAY_MACHINE_DIR/winemetal.dll"
cp -f "$WINEMETAL_SO" "$OVERLAY_UNIX_DIR/"
cp -f "$WINEMETAL_SO" "$OVERLAY_MACHINE_DIR/winemetal.so"
cp -f "$WINEMETAL_SO" "$OVERLAY_MACHINE_DIR/winemetal.dll.so"
cp -f "$WINEMETAL_SO" "$APP_DIR/winemetal.so"
cp -f "$WINEMETAL_SO" "$APP_DIR/winemetal.dll.so"
cp -f "$WINEMETAL_SO" "$PREFIX_SYSTEM32/winemetal.so"
cp -f "$WINEMETAL_SO" "$PREFIX_SYSTEM32/winemetal.dll.so"

echo "built=$EXE"
echo "prefix=$PREFIX"
echo "system32=$PREFIX_SYSTEM32"
echo "app_dir=$APP_DIR"
echo "builtin_overlay=$OVERLAY_MACHINE_DIR"
echo "unix_overlay=$OVERLAY_UNIX_DIR"
echo "dxmt_root=$OVERLAY_DIR"
echo "bind_mode=$DXMT_SMOKE_BIND_MODE"
echo "wine_builtin_dll=$WINE_BUILTIN_DLL"
echo "overrides=$DXMT_SMOKE_DLL_OVERRIDES"
echo "log=$LOG"
echo "repeat_count=$SMOKE_REPEAT_COUNT"
echo "stability_frames=$SMOKE_STABILITY_FRAMES"
echo "stability_min_ms=$SMOKE_STABILITY_MIN_MS"
echo "visible_window=$SMOKE_VISIBLE_WINDOW"

if [[ ! "$SMOKE_REPEAT_COUNT" =~ ^[0-9]+$ || "$SMOKE_REPEAT_COUNT" -lt 1 ]]; then
  echo "invalid SMOKE_REPEAT_COUNT: $SMOKE_REPEAT_COUNT" >&2
  exit 25
fi

if [[ ! "$SMOKE_STABILITY_FRAMES" =~ ^[0-9]+$ || "$SMOKE_STABILITY_FRAMES" -lt 1 ]]; then
  echo "invalid SMOKE_STABILITY_FRAMES: $SMOKE_STABILITY_FRAMES" >&2
  exit 26
fi

if [[ ! "$SMOKE_STABILITY_MIN_MS" =~ ^[0-9]+$ ]]; then
  echo "invalid SMOKE_STABILITY_MIN_MS: $SMOKE_STABILITY_MIN_MS" >&2
  exit 28
fi

if [[ "$SMOKE_VISIBLE_WINDOW" != "0" && "$SMOKE_VISIBLE_WINDOW" != "1" ]]; then
  echo "invalid SMOKE_VISIBLE_WINDOW: $SMOKE_VISIBLE_WINDOW" >&2
  exit 27
fi

SUMMARY_PATTERN="DXMTProbe|LoadLibraryExW|loaded_.*_path|GetProcAddress|CreateDXGIFactory1|factory_probe|factory_status_probe|factory_.*cookie|factory_associated_window|IDXGIFactory2|IDXGIFactory4|GetSharedResourceAdapterLuid|factory_shared_luid|RegisterStereoStatus|RegisterOcclusionStatus|MakeWindowAssociation|GetWindowAssociation|EnumAdapters|adapter_probe|RegisterClassExW|CreateWindowExW|window_create|window_visible|D3D11CreateDevice|feature_level|UnityFeatureLevelProbe|UnityFactoryProbe|CreateSwapChainForComposition|CreateSwapChainForCoreWindow|unsupported_swapchains|composition_.*swapchain|corewindow_swapchain|UnityDeviceInterfaceProbe|CheckFormatSupport|format_support|UnityFormatProbe|UnityCounterProbe|CheckCounterInfo|CheckCounter|CreateCounter|CreateSwapChainForHwnd|IDXGISwapChain::GetBuffer|GetBuffer|extra_buffer|IDXGISwapChain3|CheckColorSpaceSupport|SetColorSpace1|color_space_support|IDXGISwapChain4|SetHDRMetaData|ResizeBuffers|ResizeBuffers1|GetContainingOutput|GetDesc1|desc1|GetFullscreenDesc|fullscreen_desc|GetHwnd|GetCoreWindow|core_window|GetRestrictToOutput|restrict_output|GetBackgroundColor|SetBackgroundColor|background_rgba|GetRotation|SetRotation|rotation=|IDXGISwapChain2|GetSourceSize|SetSourceSize|source_size|GetMaximumFrameLatency|SetMaximumFrameLatency|max_frame_latency|GetMatrixTransform|SetMatrixTransform|matrix_offset|GetFullscreenState|SetFullscreenState|GetFrameStatistics|D3DCompile\\(gs_5_0\\)|CreateGeometryShader|CreateGeometryShaderWithStreamOutput|gs_bytecode_magic|Emulate stream output|CreateEmulatedVertexStreamOutputShader|GeometryShaderDraw|StreamOutputDraw|StreamOutputVerify|stream_output_staging|UnitySRGBProbe|UnityMSAAProbe|UnityResidencyProbe|UnityStabilityProbe|UnityStateProbe|UnityGammaProbe|UnityQueryProbe|UnityDeferredProbe|UnityQueryDeferredProbe|TIMESTAMP_DISJOINT|PIPELINE_STATISTICS|GetPredication|disjoint_frequency|UnityDeferredResourceProbe|UnityTessellationProbe|UnityClassLinkageProbe|UnityMRTProbe|UnityMultithreadProbe|UnityBatchProbe|CreateClassLinkage|CreateHullShader|CreateDomainShader|CreateTexture1D|CreateTexture2D|CreateShaderResourceView|CreateUnorderedAccessView|CreateBuffer\\(raw\\)|CreateBuffer\\(update_staging\\)|UpdateSubresource\\(staging_buffer|UpdateSubresource\\(staging_texture|Map\\(update_staging\\)|Map\\(update_staging_texture\\)|update_staging_word|Map\\(dynamic_vertex_no_overwrite\\)|CopySubresourceRegion|GenerateMips|mip0_rgba|mip1_rgba|ResolveSubresource|msaa4_resolve_rgba|CreateRenderTargetView|ClearRenderTargetView|CreateDepthStencilView|ClearDepthStencilView|ClearView|DiscardView|DiscardResource|OMSetRenderTargetsAndUnorderedAccessViews|Present|Present1|Readback|pixel0_bgra|pixel_readback|c0000135|err:module|not found|failed|FAIL"

SUMMARY_PATTERN="${SUMMARY_PATTERN}|UnityOutputCapabilityProbe|UnityAdapterNotificationProbe|UnityFenceProbe|D3D11On12Probe"

SMOKE_RC=0
for ((run = 1; run <= SMOKE_REPEAT_COUNT; run++)); do
  RUN_LOG="$LOG"
  if [[ "$SMOKE_REPEAT_COUNT" -gt 1 ]]; then
    RUN_LOG="$LOG_DIR/dx11-headless-${ARCH}-run${run}.log"
  fi

  set +e
  (
    env -i \
      HOME="$HOME" \
      USER="${USER:-}" \
      LOGNAME="${LOGNAME:-${USER:-}}" \
      PATH="$PATH" \
      TMPDIR="${TMPDIR:-/tmp}" \
      MACRUNNER_DXMT_ROOT="$OVERLAY_DIR" \
      WINEDLLOVERRIDES="$DXMT_SMOKE_DLL_OVERRIDES" \
      WINEDLLDIR0="$OVERLAY_DIR" \
      WINEDLLPATH="$OVERLAY_MACHINE_DIR:$OVERLAY_UNIX_DIR:$WINE_MACHINE_DIR:$WINE_UNIX_DIR" \
      WINESYSTEMDLLPATH="$OVERLAY_MACHINE_DIR" \
      WINEDEBUG="${WINEDEBUG_SMOKE:--all,+loaddll}" \
      DXMT_SMOKE_STABILITY_FRAMES="$SMOKE_STABILITY_FRAMES" \
      DXMT_SMOKE_STABILITY_MIN_MS="$SMOKE_STABILITY_MIN_MS" \
      DXMT_SMOKE_VISIBLE_WINDOW="$SMOKE_VISIBLE_WINDOW" \
      "$PROJECT_ROOT/scripts/mr-run.sh" "$WINE_DIST" "$APP_DIR/dx11_headless_smoke.exe" "$SMOKE_TIMEOUT_SECONDS"
  ) >"$RUN_LOG" 2>&1
  run_rc=$?
  set -e

  echo "run=$run/$SMOKE_REPEAT_COUNT log=$RUN_LOG"
  echo "run_exit_code=$run_rc"
  grep -E "$SUMMARY_PATTERN" "$RUN_LOG" | tail -700 || true
  if [[ "$run_rc" -ne 0 ]]; then
    SMOKE_RC="$run_rc"
    break
  fi
done

WINEPREFIX="$PREFIX" "$WINESERVER" -k >/dev/null 2>&1 || true

echo "exit_code=$SMOKE_RC"

exit "$SMOKE_RC"
