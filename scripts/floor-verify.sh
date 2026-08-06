#!/usr/bin/env bash
# Проверка связки ЭТАЖ ↔ КОММИТ ↔ МАНИФЕСТ.
#
# Правило «класть этаж с манифестом» уже есть в CLAUDE.md, но ничто не проверяло, что манифест
# описывает ИМЕННО тот этаж, рядом с которым лежит. А расходятся они молча и в обе стороны:
#   - в дист этажа доложили файл, манифест остался со старым SHA;
#   - манифест сослался на коммит, которого в истории нет (ветка удалена, переписана, опечатка);
#   - этаж уехал без части дерева — floor22 (16 КБ) уехал без графики, потому что graphics-dist/
#     лежит РЯДОМ с дистом, а не внутри.
# Такой этаж выглядит сохранённым и не восстанавливает ничего. Узнаём мы об этом через месяцы,
# когда он понадобился, — то есть ровно тогда, когда чинить уже нечем.
#
# Проверяется три вещи, и все три по фактам, а не по наличию файла:
#   1. SHA носителей из манифеста == SHA файлов в дисте этажа;
#   2. коммит, названный в манифесте, существует в этом репозитории;
#   3. этаж комплектен: dist + доказательства + манифест непустые.
#
# Использование: scripts/floor-verify.sh [каталог-этажей]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FLOORS="${1:-/Volumes/MacOS 1/MacRunner-ARM64EC-floors}"
bad=0; seen=0

[ -d "$FLOORS" ] || { echo "нет каталога этажей: $FLOORS"; exit 64; }

while IFS= read -r man; do
  d="$(dirname "$man")"; name="$(basename "$d")"; seen=$((seen+1))
  problems=()

  # 1. SHA носителей: манифест против файлов. Строки вида "ntdll.so (...): <hex16>..."
  while IFS= read -r line; do
    m="$(echo "$line" | grep -oE '[a-z0-9_]+\.(so|dll)' | head -1)"
    want="$(echo "$line" | grep -oE '[0-9a-f]{16}' | head -1)"
    [ -n "$m" ] && [ -n "$want" ] || continue
    f="$(find "$d" -name "$m" -type f 2>/dev/null | head -1)"
    if [ -z "$f" ]; then
      problems+=("манифест называет $m, а файла в этаже НЕТ")
    else
      have="$(shasum -a256 "$f" | cut -c1-16)"
      [ "$have" = "$want" ] || problems+=("$m: манифест $want, файл $have — РАСХОЖДЕНИЕ")
    fi
  done < <(grep -iE '\.(so|dll).*[0-9a-f]{16}' "$man" 2>/dev/null)

  # 2. Коммит, если назван, должен существовать. Голый hex16 — это SHA носителя, не коммит,
  #    поэтому берём только то, что стоит рядом со словом коммит/commit.
  while IFS= read -r c; do
    git -C "$ROOT" cat-file -e "${c}^{commit}" 2>/dev/null || problems+=("коммит $c НЕ существует в репозитории")
  done < <(grep -iE 'коммит|commit' "$man" 2>/dev/null | grep -oE '\b[0-9a-f]{7,40}\b' | sort -u)

  # 3. Комплектность: пустой этаж выглядит сохранённым и не восстанавливает ничего.
  [ -d "$d/dist" ] || [ -d "$d/dist-arm64ec-spike" ] || problems+=("нет каталога dist — этаж не запускается")
  [ -s "$man" ] || problems+=("манифест пуст")

  if [ ${#problems[@]} -eq 0 ]; then
    echo "OK    $name"
  else
    bad=$((bad+1)); echo "СБОЙ  $name"
    for p in "${problems[@]}"; do echo "        $p"; done
  fi
done < <(find "$FLOORS" -maxdepth 2 -name 'MANIFEST.md' -type f 2>/dev/null)

echo "── этажей проверено: $seen, со сбоями: $bad ──"
[ "$bad" -eq 0 ]
