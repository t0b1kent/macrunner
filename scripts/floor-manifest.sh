#!/usr/bin/env bash
# Заводит MANIFEST.md этажу, у которого его нет.
#
# Зачем. floor-verify.sh сверяет SHA носителей с манифестом, но манифест есть у трёх этажей из
# девятнадцати. Остальные шестнадцать не проверяются ничем: они выглядят сохранёнными, а
# восстанавливают неизвестно что. Один такой уже пойман — hk-rung13-rtv-BREAKTHROUGH-20260706,
# где манифест обещает ntdll.dll 64570dd05286610a, а лежит 022ff35787d4452d.
#
# ЧЕСТНАЯ ОГОВОРКА, которая пишется В САМ манифест и не должна теряться:
# снятый задним числом манифест фиксирует то, что лежит СЕЙЧАС. Прошлый дрейф он не покажет —
# если файл подменили месяц назад, манифест закрепит подменённый. Он предотвращает будущее
# расхождение, но не свидетельствует о прошлом. Поэтому такие манифесты помечены отдельно, и
# путать их с написанными в момент создания этажа нельзя.
#
# Использование:
#   scripts/floor-manifest.sh <каталог-этажа> [<каталог-этажа> ...]
#   scripts/floor-manifest.sh --all        — все этажи в известных хранилищах, у кого нет
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Хранилища этажей. Их ДВА, и это не случайность: том — архив, Documents — рабочие копии.
# floor-verify.sh знал только про том, поэтому девять этажей в Documents не проверялись вообще.
STORES=(
  "/Volumes/MacOS 1/MacRunner-ARM64EC-floors"
  "/Users/timurtoby/Documents/MacRunner"
)

carrier_paths() {
  local d="$1" n
  for n in ntdll xtajit xtajit64 win32u; do
    # Раскладок пять. Этажи складывались разными скриптами в разное время, и предполагать
    # одну — значит молча пропустить этаж (07.08 так потерялись шесть из девятнадцати).
    for pre in "" "dist/" "wine-dist/" "dist-arm64ec-spike/" "payload/dist/"; do
      for rel in "lib/wine/aarch64-unix/$n.so" "bin/$n.so"; do
        [ -f "$d/$pre$rel" ] && { echo "$pre$rel|$d/$pre$rel"; break 2; }
      done
    done
  done
  for n in ntdll win32u user32; do
    for pre in "" "dist/" "wine-dist/" "dist-arm64ec-spike/" "payload/dist/"; do
      [ -f "$d/${pre}lib/wine/aarch64-windows/$n.dll" ] && { echo "${pre}lib/wine/aarch64-windows/$n.dll|$d/${pre}lib/wine/aarch64-windows/$n.dll"; break; }
    done
  done
}

make_manifest() {
  local d="$1" name; name="$(basename "$d")"
  local out="$d/MANIFEST.md"
  if [ -f "$out" ]; then echo "  пропускаю (манифест уже есть): $name"; return 0; fi
  # SHA256SUMS + RESTORE.sh — формат полнее нашего: он покрывает ВСЕ файлы, а не только
  # носители. Подменять его коротким манифестом значит терять охват.
  if [ -f "$d/SHA256SUMS" ]; then echo "  пропускаю (есть SHA256SUMS, охват шире): $name"; return 0; fi

  local rows; rows="$(carrier_paths "$d")"
  if [ -z "$rows" ]; then echo "  ПРОПУСК (не нашёл ни одного носителя): $name"; return 1; fi

  # Описание, если этаж его нёс — перенести в манифест, чтобы смысл и суммы лежали вместе.
  local desc=""
  for c in "ЧТО-ВНУТРИ.txt" "README-ЧТО-ЭТО.md" "ОПИСАНИЕ.txt"; do
    [ -f "$d/$c" ] && { desc="$d/$c"; break; }
  done

  {
    echo "# $name"
    echo
    echo "**Манифест снят задним числом $(date '+%Y-%m-%d %H:%M').** Он фиксирует то, что лежит в"
    echo "этаже СЕЙЧАС. Если файл подменили раньше — здесь закреплён подменённый: прошлый дрейф"
    echo "такой манифест не показывает, он защищает только от будущего. Манифесты, написанные в"
    echo "момент создания этажа, свидетельствуют о составе; этот — нет."
    echo
    if [ -n "$desc" ]; then
      echo "**Описание этажа** (перенесено из \`$(basename "$desc")\`):"
      echo
      sed 's/^/> /' "$desc"
      echo
    else
      echo "**Описание:** этаж его не нёс. Чем он отличается от соседних — не записано нигде."
      echo
    fi
    # ПУТЬ, А НЕ ТОЛЬКО ИМЯ. 07.08 первая редакция писала «ntdll.dll: <sha>», а проверка искала
    # файл через find -name и брала ПЕРВЫЙ попавшийся — в этаже копий несколько. Восемнадцать
    # этажей объявились сломанными, хотя сверялись разные файлы. Путь снимает двусмысленность.
    echo "**Состав носителей** (путь относительно каталога этажа):"
    printf '%s\n' "$rows" | while IFS='|' read -r nm p; do
      echo "- $nm: $(shasum -a256 "$p" | cut -c1-16)"
    done
    echo
    echo "**Размер:** $(du -sh "$d" 2>/dev/null | cut -f1)"
  } > "$out"
  echo "  создан: $name ($(printf '%s\n' "$rows" | wc -l | tr -d ' ') носителей)"
}

targets=()
if [ "${1:-}" = "--all" ]; then
  for s in "${STORES[@]}"; do
    [ -d "$s" ] || { echo "хранилище недоступно, пропускаю: $s"; continue; }
    for d in "$s"/*/; do
      [ -d "$d" ] || continue
      case "$(basename "$d")" in Main|Cross|game-*|reports) continue;; esac
      # этажом считаем каталог, в котором есть хоть один носитель
      [ -n "$(carrier_paths "$d")" ] && targets+=("${d%/}")
    done
  done
else
  [ $# -ge 1 ] || { echo "нужен каталог этажа или --all"; exit 64; }
  targets=("$@")
fi

echo "── завожу манифесты: ${#targets[@]} кандидатов ──"
for d in "${targets[@]}"; do make_manifest "$d"; done
