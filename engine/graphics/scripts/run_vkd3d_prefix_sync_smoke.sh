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
  done

  echo "arch=$arch sync_log=$sync_log vkd3d=PASS dxmt_dependency=PASS"
done

echo "vkd3d_prefix_sync_result=PASS"
