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
# ХРАНИЛИЩ ДВА. Раньше здесь был один путь на том, и девять этажей в Documents/MacRunner —
# включая floor26-ЭКРАН-ЯЗЫКА, floor25, floor24, floor23 — не проверялись НИЧЕМ. Они выглядели
# сохранёнными и восстановили бы неизвестно что. Аргументом можно задать свой путь.
if [ $# -ge 1 ]; then STORES=("$1"); else
  STORES=("/Volumes/MacOS 1/MacRunner-ARM64EC-floors" "/Users/timurtoby/Documents/MacRunner")
fi
bad=0; seen=0; skipped=0

have_store=0
for s in "${STORES[@]}"; do [ -d "$s" ] && have_store=1; done
[ "$have_store" = 1 ] || { echo "ни одно хранилище этажей недоступно: ${STORES[*]}"; exit 64; }
for s in "${STORES[@]}"; do
  [ -d "$s" ] || echo "  хранилище недоступно, пропускаю: $s"
done

while IFS= read -r man; do
  d="$(dirname "$man")"; name="$(basename "$d")"; seen=$((seen+1))
  problems=()

  # 1. SHA носителей: манифест против файлов. Строки вида "ntdll.so (...): <hex16>..."
  while IFS= read -r line; do
    # Если манифест назвал ПУТЬ — сверяем именно его. find -name брал первый попавшийся файл
    # с таким именем, а копий в этаже несколько: 07.08 это дало 18 ложных «расхождений».
    rel="$(echo "$line" | grep -oE '[A-Za-z0-9_./-]+/[a-z0-9_]+\.(so|dll)' | head -1)"
    m="$(echo "$line" | grep -oE '[a-z0-9_]+\.(so|dll)' | head -1)"
    want="$(echo "$line" | grep -oE '[0-9a-f]{16}' | head -1)"
    [ -n "$m" ] && [ -n "$want" ] || continue
    if [ -n "$rel" ] && [ -f "$d/$rel" ]; then f="$d/$rel"; m="$rel"
    else f="$(find "$d" -name "$m" -type f 2>/dev/null | head -1)"; fi
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
  # Раскладок пять, см. floor-manifest.sh. Проверять одну — значит объявить сломанным этаж,
  # который цел, просто сложен другим скриптом.
  ok_layout=0
  for l in dist dist-arm64ec-spike wine-dist payload/dist lib/wine; do
    [ -d "$d/$l" ] && ok_layout=1
  done
  [ "$ok_layout" = 1 ] || problems+=("не нашёл дист ни в одной из известных раскладок — этаж не запускается")
  [ -s "$man" ] || problems+=("манифест пуст")

  if [ ${#problems[@]} -eq 0 ]; then
    echo "OK    $name"
  else
    bad=$((bad+1)); echo "СБОЙ  $name"
    for p in "${problems[@]}"; do echo "        $p"; done
  fi
done < <(for s in "${STORES[@]}"; do [ -d "$s" ] && find "$s" -maxdepth 2 -name 'MANIFEST.md' -type f 2>/dev/null; done)

# Этажи формата SHA256SUMS проверяются своим средством — оно покрывает ВСЕ файлы, а не только
# носители, и подменять его нашей короткой сверкой значило бы потерять охват.
while IFS= read -r sums; do
  d="$(dirname "$sums")"; name="$(basename "$d")"; seen=$((seen+1))
  # РАЗЛИЧАТЬ «хеш не сошёлся» и «файла нет по записанному пути». Второе почти всегда значит,
  # что опись писалась с АБСОЛЮТНЫМИ путями, а этаж потом переехал (hk-rung11-reproducible уехал
  # из artifacts/ на том). Это не порча, а непереносимая опись, и объявлять её сбоем — врать.
  # Ровно эта путаница 07.08 дала 3 «расхождения» на целом этаже.
  out="$( cd "$d" && shasum -a256 -c SHA256SUMS 2>/dev/null )"
  miss=$(printf '%s\n' "$out" | grep -c 'FAILED open or read')
  diff=$(printf '%s\n' "$out" | grep 'FAILED' | grep -vc 'open or read')
  txt=$(printf '%s\n' "$out" | grep 'FAILED' | grep -vc 'open or read.*\.\(so\|dll\)$')
  if [ "$diff" = 0 ] && [ "$miss" = 0 ]; then
    echo "OK    $name (SHA256SUMS, все файлы)"
  elif [ "$diff" = 0 ]; then
    echo "ОПИСЬ НЕПЕРЕНОСИМА  $name: $miss записей с путями, которых здесь нет (этаж переехал)"
  else
    bad=$((bad+1))
    echo "СБОЙ  $name (SHA256SUMS): $diff файлов с ДРУГИМ содержимым"
    printf '%s\n' "$out" | grep 'FAILED' | grep -v 'open or read' | head -4 | sed 's|^|        |'
  fi
done < <(for s in "${STORES[@]}"; do [ -d "$s" ] && find "$s" -maxdepth 2 -name 'SHA256SUMS' -type f 2>/dev/null; done)

# Этаж без всякой описи — тоже результат, и молчать о нём нельзя.
while IFS= read -r d; do
  [ -f "$d/MANIFEST.md" ] || [ -f "$d/SHA256SUMS" ] && continue
  # Этажом считаем каталог, где есть НАШ носитель, а не просто lib/. Иначе в отчёт лезут
  # посторонние проекты, лежащие в том же Documents (lampa-tizen, tizen-hdrezka).
  for l in dist dist-arm64ec-spike wine-dist payload/dist; do
    if [ -f "$d/$l/lib/wine/aarch64-unix/ntdll.so" ] || [ -f "$d/lib/wine/aarch64-unix/ntdll.so" ]; then
      skipped=$((skipped+1)); echo "БЕЗ ОПИСИ  $(basename "$d")"; break
    fi
  done
done < <(for s in "${STORES[@]}"; do [ -d "$s" ] && find "$s" -maxdepth 1 -mindepth 1 -type d 2>/dev/null; done)

echo "── этажей проверено: $seen, со сбоями: $bad, без описи: $skipped ──"
[ "$bad" -eq 0 ]
