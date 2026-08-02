#!/usr/bin/env bash
# Preflight for engine runs. Refuses to spend 4 minutes proving nothing.
#
# ── Why this exists (each check is a failure that actually happened) ─────────────────────────
# 1. A five-run patch-count bisect was launched without MACRUNNER_HB_BLOCK_CHAIN=1, the master
#    gate, which defaults OFF. All five arms ran with chaining disabled and reported "no wedge at
#    any N". The numbers looked like an answer and were an artefact of a feature that was never on.
# 2. An A/B compared two different binaries built at different times with different printing, so
#    the arms were not comparable at all.
# 3. Counters have four times decided an outcome instead of the thing being measured: a period so
#    coarse it never printed, a predicate never reached because of an upstream short circuit, and
#    twice a marker whose name did not match what the code actually prints.
#
# So: a gate you cannot prove is active is a gate that is off, and a marker you cannot prove is
# printed is a marker that does not exist. Both are checked here, BEFORE the long run.
#
# Usage:
#   scripts/preflight-run.sh <tag> <secs> <expect-markers-csv> <ENV=VAL> [ENV=VAL ...]
# Example:
#   scripts/preflight-run.sh chain 200 'chainedge,chain-RBP-VIOLATION' \
#       MACRUNNER_HB_BLOCK_CHAIN=1 MACRUNNER_HB_TRACE_CHAIN_EDGE=1
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
SRC="engine/hyperbridge/src"
DIST="engine/wine/dist-arm64ec-spike/lib/wine/aarch64-unix/xtajit64.so"

[ $# -ge 4 ] || { echo "usage: $0 <tag> <secs> <markers-csv> <ENV=VAL>..."; exit 64; }
TAG="$1"; SECS="$2"; MARKERS="$3"; shift 3
ENVS=("$@")
fail=0
note() { echo "  $*"; }

echo "── preflight: $TAG ──"

# ── 1. Every gate name must exist in the source. Catches typos and invented variables. ────────
for kv in "${ENVS[@]}"; do
  name="${kv%%=*}"
  case "$name" in MACRUNNER_*) ;; *) continue;; esac
  if ! grep -rqs -- "$name" "$SRC"; then
    note "FAIL: гейт $name не встречается в исходниках — опечатка или выдуманное имя"
    fail=1
  fi
done

# ── 2. The gate must be present in the DEPLOYED binary, not just in the tree. ─────────────────
# A publish step can die silently and leave the old .so in place; the tree then proves nothing.
for kv in "${ENVS[@]}"; do
  name="${kv%%=*}"
  case "$name" in MACRUNNER_*) ;; *) continue;; esac
  if ! strings "$DIST" 2>/dev/null | grep -qs -- "$name"; then
    note "FAIL: $name отсутствует в РАЗВЁРНУТОМ $DIST — собрано, но не задеплоено"
    fail=1
  fi
done

# ── 3. Master-gate dependencies. This is the check that would have saved the bisect. ──────────
have() { for kv in "${ENVS[@]}"; do [ "${kv%%=*}" = "$1" ] && [ "${kv#*=}" != "0" ] && return 0; done; return 1; }
uses_chain=0
for kv in "${ENVS[@]}"; do case "${kv%%=*}" in MACRUNNER_HB_CHAIN_*) uses_chain=1;; esac; done
# A control arm that disables chaining ON PURPOSE is legitimate, and must be distinguishable from
# forgetting the master gate -- which is the whole point of this check. PREFLIGHT_CHAIN_OFF_OK=1
# is that distinction: intent has to be stated, not inferred.
if [ "${PREFLIGHT_CHAIN_OFF_OK:-0}" = 1 ]; then
  note "контрольная рука: сцепление выключено НАМЕРЕННО (PREFLIGHT_CHAIN_OFF_OK=1)"
elif [ "$uses_chain" = 1 ] && ! have MACRUNNER_HB_BLOCK_CHAIN; then
  note "FAIL: выставлены MACRUNNER_HB_CHAIN_*, но мастер-гейт MACRUNNER_HB_BLOCK_CHAIN не включён."
  note "      Он по умолчанию 0. Без него сцепление не работает и прогон измерит выключенную функцию."
  fail=1
fi

# ── 4. EVERY carrier of libhyperbridge.a must be current, not just the one you remembered. ────
# This is the check that cost a day. libhyperbridge.a is linked into THREE modules -- ntdll.so,
# xtajit.so and xtajit64.so -- and deploying only xtajit64.so leaves the engine code that actually
# executes (ntdll's copy) at whatever version it was. Every run then silently exercises old code
# while the dist looks freshly deployed. Verified by tagging a print: the log carried the untagged
# text 7232 times while the deployed xtajit64.so contained only the tagged one.
CARRIERS=$(grep -rls "libhyperbridge.a" engine/wine/dlls/*/Makefile.in 2>/dev/null \
           | sed 's|engine/wine/dlls/||; s|/Makefile.in||')
[ -n "$CARRIERS" ] || { note "FAIL: не нашёл ни одного модуля, линкующего libhyperbridge.a"; fail=1; }
for m in $CARRIERS; do
  so="engine/wine/dist-arm64ec-spike/lib/wine/aarch64-unix/$m.so"
  if [ ! -f "$so" ]; then note "FAIL: носитель $m.so отсутствует в dist"; fail=1; continue; fi
  stale=$(find "$SRC" engine/hyperbridge/include \( -name '*.c' -o -name '*.h' \) 2>/dev/null \
          | while read -r f; do [ "$f" -nt "$so" ] && echo "$f" && break; done)
  if [ -n "$stale" ]; then
    note "FAIL: $m.so СТАРШЕ исходников — этот носитель не пересобран (правки не в прогоне)"
    note "      первый новее: $stale"
    fail=1
  else
    note "носитель $m.so: свежий ($(shasum -a256 "$so" | cut -c1-12))"
  fi
done

[ "$fail" = 0 ] || { echo "── ПРОГОН НЕ ЗАПУЩЕН: устраните причины выше ──"; exit 2; }
note "OK: гейты существуют, задеплоены, мастер-гейт на месте, бинарь свежее исходников"
note "dist=$(shasum -a256 "$DIST" | cut -c1-16)"

# ── 5. Run, then prove the evidence markers actually appeared. ────────────────────────────────
while [ "$(ps -Ao comm | grep -cE 'Hollow Knight\.exe|AbzuGame')" -gt 0 ]; do sleep 10; done
sleep 5
env "${ENVS[@]}" MACRUNNER_HB_CACHE_RELOC=1 MACRUNNER_HB_TRANSLATION_CACHE=1 \
    ./scripts/hk-run-try12-config.sh "$TAG" "$SECS" 1 >/dev/null 2>&1
D=$(ls -dt reports/phase4-hollow-knight/*"$TAG"* 2>/dev/null | head -1)
echo "── постпроверка: $D ──"
miss=0
IFS=',' read -ra MS <<< "$MARKERS"
for m in "${MS[@]}"; do
  [ -z "$m" ] && continue
  n=$(python3 - "$D/run.log" "$m" <<'PY'
import sys
try: print(sum(1 for l in open(sys.argv[1],'rb') if sys.argv[2].encode() in l))
except Exception: print(0)
PY
)
  if [ "$n" = "0" ]; then note "ОТСУТСТВУЕТ маркер '$m' — доказательства нет, результат читать нельзя"; miss=1
  else note "маркер '$m': $n"; fi
done
[ "$miss" = 0 ] || { echo "── РЕЗУЛЬТАТ НЕДЕЙСТВИТЕЛЕН: ожидаемые улики не напечатались ──"; exit 3; }
echo "── прогон валиден: $D ──"
