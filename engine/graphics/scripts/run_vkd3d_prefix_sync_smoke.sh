#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="${PROJECT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
source "$PROJECT_ROOT/config/env.sh"

WINE_DIST="${VKD3D_PREFIX_SYNC_WINE_DIST:-$PROJECT_ROOT/engine/wine/dist-arm64ec-spike}"
LOG_DIR="$PROJECT_ROOT/artifacts/vkd3d-prefix-sync"
STAMP="$(date +%Y%m%d-%H%M%S)"
RUN_DIR="$LOG_DIR/run-$STAMP"
VKD3D_MODULES=(d3d12.dll d3d12core.dll)
DXMT_MODULES=(dxgi.dll winemetal.dll)
OBJDUMP="${VKD3D_PREFIX_SYNC_OBJDUMP:-$(command -v objdump || true)}"

mkdir -p "$RUN_DIR"
"$PROJECT_ROOT/scripts/disk-guard.sh" --check-only >/dev/null

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

if (($#)); then
  RAW_ARCHES=("$@")
else
  RAW_ARCHES=(aarch64-windows x86_64-windows)
fi

echo "run_dir=$RUN_DIR"

for raw_arch in "${RAW_ARCHES[@]}"; do
  arch="$(normalize_arch "$raw_arch")"
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
    case "$module" in
      d3d12.dll)
        check_exports "vkd3d/$arch/$module" "$system32/$module" \
          D3D12CreateDevice \
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
  done

  echo "arch=$arch sync_log=$sync_log vkd3d=PASS dxmt_dependency=PASS"
done

echo "vkd3d_prefix_sync_result=PASS"
