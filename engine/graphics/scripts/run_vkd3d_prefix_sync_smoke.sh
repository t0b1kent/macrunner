#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="${PROJECT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
source "$PROJECT_ROOT/config/env.sh"
source "$PROJECT_ROOT/engine/graphics/build-support/common.sh"

WINE_DIST="${VKD3D_PREFIX_SYNC_WINE_DIST:-$PROJECT_ROOT/engine/wine/dist-arm64ec-spike}"
RUNTIME_WINE_DIST="${VKD3D_PREFIX_SYNC_RUNTIME_WINE_DIST:-$PROJECT_ROOT/engine/wine/dist}"
LOG_DIR="$PROJECT_ROOT/artifacts/vkd3d-prefix-sync"
STAMP="$(date +%Y%m%d-%H%M%S)"
RUN_DIR="$LOG_DIR/run-$STAMP"
VKD3D_MODULES=(d3d12.dll d3d12core.dll)
DXMT_MODULES=(dxgi.dll winemetal.dll)
OBJDUMP="${VKD3D_PREFIX_SYNC_OBJDUMP:-$(command -v objdump || true)}"
RUNTIME_PROBE_SOURCE="$PROJECT_ROOT/engine/graphics/tests/vkd3d/vkd3d_load_probe.c"
RUNTIME_PROBE_ENABLED="${VKD3D_PREFIX_SYNC_RUNTIME:-1}"
RUNTIME_PROBE_ARCHES="${VKD3D_PREFIX_SYNC_RUNTIME_ARCHES:-aarch64-windows}"
RUNTIME_PROBE_TIMEOUT_SECONDS="${VKD3D_PREFIX_SYNC_RUNTIME_TIMEOUT_SECONDS:-45}"

mkdir -p "$RUN_DIR"
"$PROJECT_ROOT/scripts/disk-guard.sh" --check-only >/dev/null

detect_vkd3d_source() {
  if [[ -n "${VKD3D_PREFIX_SYNC_SOURCE:-}" ]]; then
    echo "$VKD3D_PREFIX_SYNC_SOURCE"
  elif [[ -d "$PROJECT_ROOT/engine/vkd3d-proton" ]]; then
    echo "$PROJECT_ROOT/engine/vkd3d-proton"
  else
    echo "$PROJECT_ROOT/engine/vkd3d"
  fi
}

normalize_arch() {
  case "$1" in
    aarch64|arm64|aarch64-windows) echo "aarch64-windows" ;;
    x86_64|amd64|x64|x86_64-windows) echo "x86_64-windows" ;;
    *) echo "unsupported vkd3d prefix sync arch: $1" >&2; exit 2 ;;
  esac
}

check_module() {
  local label="$1"
  local expected="$2"
  local actual="$3"

  if [[ ! -r "$expected" ]]; then
    echo "vkd3d_prefix_sync_result=FAIL reason=missing_expected module=$label path=$expected"
    exit 2
  fi
  if [[ ! -r "$actual" ]]; then
    echo "vkd3d_prefix_sync_result=FAIL reason=missing_prefix_copy module=$label path=$actual"
    exit 1
  fi
  if ! cmp -s "$expected" "$actual"; then
    echo "vkd3d_prefix_sync_result=FAIL reason=mismatched_prefix_copy module=$label"
    echo "expected=$expected"
    echo "actual=$actual"
    exit 1
  fi
}

check_exports() {
  local label="$1"
  local path="$2"
  shift 2
  local export_log="$RUN_DIR/exports-${label//\//_}.log"

  if [[ -z "$OBJDUMP" || ! -x "$OBJDUMP" ]]; then
    echo "vkd3d_prefix_sync_result=FAIL reason=missing_objdump module=$label"
    exit 2
  fi

  if ! "$OBJDUMP" -p "$path" >"$export_log" 2>&1; then
    echo "vkd3d_prefix_sync_result=FAIL reason=export_parse_failed module=$label log=$export_log"
    exit 1
  fi

  for symbol in "$@"; do
    if ! grep -Eq "[[:space:]]${symbol}([[:space:]]|$)" "$export_log"; then
      echo "vkd3d_prefix_sync_result=FAIL reason=missing_export module=$label symbol=$symbol log=$export_log"
      exit 1
    fi
  done

  echo "exports=$label PASS symbols=$* log=$export_log"
}

check_machine() {
  local label="$1"
  local path="$2"
  local expected="$3"
  local machine_log="$RUN_DIR/machine-${label//\//_}.log"

  if ! "$OBJDUMP" -f "$path" >"$machine_log" 2>&1; then
    echo "vkd3d_prefix_sync_result=FAIL reason=machine_parse_failed module=$label log=$machine_log"
    exit 1
  fi

  if ! grep -Eq "architecture:[[:space:]]*$expected([[:space:]]|$)" "$machine_log"; then
    echo "vkd3d_prefix_sync_result=FAIL reason=unexpected_machine module=$label expected=$expected log=$machine_log"
    exit 1
  fi

  echo "machine=$label PASS architecture=$expected log=$machine_log"
}

check_structured_store_source_gate() {
  local source_path source_name hlsl_codegen source_log

  source_path="$(detect_vkd3d_source)"
  source_name="$(basename "$source_path")"
  hlsl_codegen="$source_path/libs/vkd3d-shader/hlsl_codegen.c"
  source_log="$RUN_DIR/structured-store-source.log"

  {
    echo "source=$source_path"
    echo "source_name=$source_name"
    if [[ -f "$hlsl_codegen" ]]; then
      echo "hlsl_codegen=$hlsl_codegen"
    else
      echo "hlsl_codegen=absent"
    fi
  } >"$source_log"

  if [[ ! -d "$source_path" ]]; then
    echo "vkd3d_prefix_sync_result=FAIL reason=missing_vkd3d_source path=$source_path log=$source_log"
    exit 2
  fi

  if [[ ! -f "$hlsl_codegen" ]]; then
    if [[ "$source_name" == "vkd3d-proton" ]]; then
      echo "structured_store_source=PASS mode=active_source_no_hlsl_codegen log=$source_log"
      return
    fi
    echo "vkd3d_prefix_sync_result=FAIL reason=missing_hlsl_codegen source=$source_name log=$source_log"
    exit 2
  fi

  if grep -q "Structured buffers store is not implemented" "$hlsl_codegen"; then
    echo "vkd3d_prefix_sync_result=FAIL reason=structured_store_fixme_present source=$source_name log=$source_log"
    exit 1
  fi

  if ! grep -q "VSIR_OP_STORE_STRUCTURED, 1, 3" "$hlsl_codegen"; then
    echo "vkd3d_prefix_sync_result=FAIL reason=missing_structured_store_lowering source=$source_name log=$source_log"
    exit 1
  fi

  echo "structured_store_source=PASS mode=hlsl_codegen_lowering source=$source_name log=$source_log"
}

runtime_probe_arch_enabled() {
  local arch="$1"
  local enabled_arch

  [[ "$RUNTIME_PROBE_ENABLED" == "1" ]] || return 1
  for enabled_arch in $RUNTIME_PROBE_ARCHES; do
    [[ "$(normalize_arch "$enabled_arch")" == "$arch" ]] && return 0
  done
  return 1
}

compile_runtime_loader_probe() {
  local arch="$1"
  local raw_arch triplet compiler exe

  case "$arch" in
    aarch64-windows) raw_arch="aarch64" ;;
    x86_64-windows) raw_arch="x86_64" ;;
    *) echo "unsupported runtime probe arch: $arch" >&2; exit 2 ;;
  esac

  ensure_llvm_mingw
  triplet="$(triplet_for_arch "$raw_arch")"
  compiler="$TOOLCHAIN_BIN/$triplet-clang"
  exe="$RUN_DIR/vkd3d-load-probe-$arch.exe"

  "$compiler" -O2 -Wall -Wextra -o "$exe" "$RUNTIME_PROBE_SOURCE"
  echo "$exe"
}

run_runtime_loader_probe() {
  local arch="$1"
  local prefix="$2"
  local exe="$3"
  local app_dir="$RUN_DIR/runtime-app-$arch"
  local app_exe="$app_dir/vkd3d_load_probe.exe"
  local log="$RUN_DIR/runtime-loader-$arch.log"
  local unix_arch
  local wine="$RUNTIME_WINE_DIST/bin/wine"
  local wineserver="$RUNTIME_WINE_DIST/bin/wineserver"
  local wine_unix_lib
  local dyld_library_path="${DYLD_LIBRARY_PATH:-}"
  local dyld_fallback_library_path="${DYLD_FALLBACK_LIBRARY_PATH:-}"
  local runtime_winedllpath
  local runtime_systemdllpath
  local timeout_bin=""
  local child
  local elapsed
  local rc=0

  case "$arch" in
    aarch64-windows) unix_arch="aarch64-unix" ;;
    x86_64-windows) unix_arch="aarch64-unix" ;;
    *) echo "unsupported runtime probe arch: $arch" >&2; exit 2 ;;
  esac
  runtime_winedllpath="$PROJECT_ROOT/engine/graphics/dist/vkd3d/$arch"
  runtime_winedllpath="$runtime_winedllpath:$PROJECT_ROOT/engine/graphics/dist/dxmt/$arch"
  runtime_winedllpath="$runtime_winedllpath:$PROJECT_ROOT/engine/graphics/dist/dxmt/$unix_arch"
  runtime_winedllpath="$runtime_winedllpath:$RUNTIME_WINE_DIST/lib/wine/$arch"
  runtime_winedllpath="$runtime_winedllpath:$RUNTIME_WINE_DIST/lib/wine/$unix_arch"
  runtime_systemdllpath="$PROJECT_ROOT/engine/graphics/dist/vkd3d/$arch"
  runtime_systemdllpath="$runtime_systemdllpath:$PROJECT_ROOT/engine/graphics/dist/dxmt/$arch"
  wine_unix_lib="$RUNTIME_WINE_DIST/lib/wine/$unix_arch"

  mkdir -p "$app_dir"
  cp -f "$exe" "$app_exe"
  cp -f "$PROJECT_ROOT/engine/graphics/dist/vkd3d/$arch/d3d12.dll" "$app_dir/d3d12.dll"
  cp -f "$PROJECT_ROOT/engine/graphics/dist/vkd3d/$arch/d3d12core.dll" "$app_dir/d3d12core.dll"
  cp -f "$PROJECT_ROOT/engine/graphics/dist/dxmt/$arch/dxgi.dll" "$app_dir/dxgi.dll"
  cp -f "$PROJECT_ROOT/engine/graphics/dist/dxmt/$arch/winemetal.dll" "$app_dir/winemetal.dll"
  if [[ -f "$PROJECT_ROOT/engine/graphics/dist/dxmt/$unix_arch/winemetal.so" ]]; then
    cp -f "$PROJECT_ROOT/engine/graphics/dist/dxmt/$unix_arch/winemetal.so" "$app_dir/winemetal.so"
    cp -f "$PROJECT_ROOT/engine/graphics/dist/dxmt/$unix_arch/winemetal.so" "$app_dir/winemetal.dll.so"
  fi

  if [[ -d "$wine_unix_lib" ]]; then
    dyld_library_path="$wine_unix_lib${dyld_library_path:+:$dyld_library_path}"
    dyld_fallback_library_path="$wine_unix_lib${dyld_fallback_library_path:+:$dyld_fallback_library_path}"
  fi

  {
    echo "runtime_loader_arch=$arch"
    echo "runtime_loader_exe=$app_exe"
    echo "runtime_loader_app_dir=$app_dir"
    echo "runtime_loader_overrides=d3d12,d3d12core=n"
    echo "runtime_loader_wine_dist=$RUNTIME_WINE_DIST"
    echo "runtime_loader_prefix=$prefix"
    echo "runtime_loader_winedllpath=$runtime_winedllpath"
    echo "runtime_loader_winesystemdllpath=$runtime_systemdllpath"
  } >"$log"

  if command -v timeout >/dev/null 2>&1; then
    timeout_bin="$(command -v timeout)"
  elif command -v gtimeout >/dev/null 2>&1; then
    timeout_bin="$(command -v gtimeout)"
  fi

  set +e
  if [[ -n "$timeout_bin" ]]; then
    (
      cd "$app_dir" &&
      WINEPREFIX="$prefix" \
      WINEDLLOVERRIDES="d3d12,d3d12core=n" \
      WINEDEBUG="-all,+loaddll" \
      WINELOADERNOEXEC="${WINELOADERNOEXEC:-1}" \
      MACRUNNER_GRAPHICS_BACKEND="dxmt" \
      MACRUNNER_DXMT_ROOT="$PROJECT_ROOT/engine/graphics/dist/dxmt" \
      MACRUNNER_PREFIX_SYSTEM32_ARCH="$arch" \
      WINEDLLPATH="$runtime_winedllpath" \
      WINESYSTEMDLLPATH="$runtime_systemdllpath" \
      DYLD_LIBRARY_PATH="$dyld_library_path" \
      DYLD_FALLBACK_LIBRARY_PATH="$dyld_fallback_library_path" \
        "$timeout_bin" "$RUNTIME_PROBE_TIMEOUT_SECONDS" "$wine" \
        "vkd3d_load_probe.exe"
    ) >>"$log" 2>&1
    rc=$?
  else
    (
      cd "$app_dir" &&
      WINEPREFIX="$prefix" \
      WINEDLLOVERRIDES="d3d12,d3d12core=n" \
      WINEDEBUG="-all,+loaddll" \
      WINELOADERNOEXEC="${WINELOADERNOEXEC:-1}" \
      MACRUNNER_GRAPHICS_BACKEND="dxmt" \
      MACRUNNER_DXMT_ROOT="$PROJECT_ROOT/engine/graphics/dist/dxmt" \
      MACRUNNER_PREFIX_SYSTEM32_ARCH="$arch" \
      WINEDLLPATH="$runtime_winedllpath" \
      WINESYSTEMDLLPATH="$runtime_systemdllpath" \
      DYLD_LIBRARY_PATH="$dyld_library_path" \
      DYLD_FALLBACK_LIBRARY_PATH="$dyld_fallback_library_path" \
        "$wine" "vkd3d_load_probe.exe"
    ) >>"$log" 2>&1 &
    child=$!
    rc=124
    elapsed=0
    while kill -0 "$child" 2>/dev/null; do
      if [[ "$elapsed" -ge "$RUNTIME_PROBE_TIMEOUT_SECONDS" ]]; then
        kill "$child" 2>/dev/null || true
        sleep 1
        kill -9 "$child" 2>/dev/null || true
        wait "$child" 2>/dev/null || true
        break
      fi
      sleep 1
      elapsed=$((elapsed + 1))
    done
    if ! kill -0 "$child" 2>/dev/null; then
      wait "$child" 2>/dev/null
      rc=$?
    fi
  fi
  set -e

  WINEPREFIX="$prefix" \
  DYLD_LIBRARY_PATH="$dyld_library_path" \
  DYLD_FALLBACK_LIBRARY_PATH="$dyld_fallback_library_path" \
    "$wineserver" -k >/dev/null 2>&1 || true

  if [[ "$rc" -ne 0 ]]; then
    echo "vkd3d_prefix_sync_result=FAIL reason=runtime_loader_exit arch=$arch rc=$rc log=$log"
    exit 1
  fi
  if ! grep -q "vkd3d_runtime_load_result=PASS" "$log"; then
    echo "vkd3d_prefix_sync_result=FAIL reason=runtime_loader_missing_pass arch=$arch log=$log"
    exit 1
  fi

  echo "runtime_loader=$arch PASS log=$log"
}

if (($#)); then
  RAW_ARCHES=("$@")
else
  RAW_ARCHES=(aarch64-windows x86_64-windows)
fi

echo "run_dir=$RUN_DIR"
check_structured_store_source_gate

for raw_arch in "${RAW_ARCHES[@]}"; do
  arch="$(normalize_arch "$raw_arch")"
  case "$arch" in
    aarch64-windows) expected_machine="aarch64" ;;
    x86_64-windows) expected_machine="x86_64" ;;
  esac
  prefix="$RUN_DIR/prefix-$arch"
  system32="$prefix/drive_c/windows/system32"
  sync_log="$RUN_DIR/sync-$arch.log"

  "$PROJECT_ROOT/scripts/sync-prefix-from-dist.sh" \
    --dist "$WINE_DIST" \
    --prefix "$prefix" \
    --system32-arch "$arch" >"$sync_log" 2>&1

  for module in "${VKD3D_MODULES[@]}"; do
    check_module "vkd3d/$arch/$module" \
      "$PROJECT_ROOT/engine/graphics/dist/vkd3d/$arch/$module" \
      "$system32/$module"
    check_machine "vkd3d/$arch/$module" "$system32/$module" "$expected_machine"
    case "$module" in
      d3d12.dll)
        check_exports "vkd3d/$arch/$module" "$system32/$module" \
          D3D12CreateDevice \
          D3D12CreateRootSignatureDeserializer \
          D3D12CreateVersionedRootSignatureDeserializer \
          D3D12EnableExperimentalFeatures \
          D3D12GetDebugInterface \
          D3D12GetInterface \
          D3D12SerializeRootSignature \
          D3D12SerializeVersionedRootSignature
        ;;
      d3d12core.dll)
        check_exports "vkd3d/$arch/$module" "$system32/$module" \
          D3D12GetInterface \
          D3D12SDKVersion
        ;;
    esac
  done

  for module in "${DXMT_MODULES[@]}"; do
    check_module "dxmt/$arch/$module" \
      "$PROJECT_ROOT/engine/graphics/dist/dxmt/$arch/$module" \
      "$system32/$module"
    check_machine "dxmt/$arch/$module" "$system32/$module" "$expected_machine"
    case "$module" in
      dxgi.dll)
        check_exports "dxmt/$arch/$module" "$system32/$module" \
          CreateDXGIFactory \
          CreateDXGIFactory1 \
          CreateDXGIFactory2 \
          DXGIGetDebugInterface1
        ;;
    esac
  done

  if runtime_probe_arch_enabled "$arch"; then
    probe_exe="$(compile_runtime_loader_probe "$arch")"
    run_runtime_loader_probe "$arch" "$prefix" "$probe_exe"
  fi

  echo "arch=$arch sync_log=$sync_log vkd3d=PASS dxmt_dependency=PASS"
done

echo "vkd3d_prefix_sync_result=PASS"
