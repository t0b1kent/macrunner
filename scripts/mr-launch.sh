#!/usr/bin/env bash
# ЗАПУСК ДЛЯ ОПЕРАТОРА: одна команда, которая сама приводит профиль в рабочее состояние.
#
# Зачем отдельно от mr-profile.sh. Тот — рабочий инструмент: делает ровно что сказано и падает,
# если что-то не так. Этот — для двойного клика: сам чинит то, что чинится однозначно, и
# показывает результат словами, а не кодом возврата.
#
# Что он снимает с оператора (каждый пункт стоил времени 07.08):
#
# 1. КАРАНТИН macOS. Распакованный Keka архив несёт com.apple.quarantine на всех файлах, наши
#    .dylib подписаны ad-hoc, и Gatekeeper отказывает МОЛЧА — отказ происходит в системе, выше
#    нашего кода, и в run.log про него нет ни строки. Возвращается при КАЖДОЙ распаковке архива,
#    так что снимать надо перед каждым запуском, а не однократно.
#    Обязательно -s: без него xattr идёт по симлинку dosdevices/z: -> / и упирается в SIP на
#    корне тома, обрываясь с «Operation not permitted».
#
# 2. ВЫБОР ПРОФИЛЯ. Ярлык, лежащий ВНУТРИ профиля, исчезает при перераспаковке архива. Этот
#    живёт снаружи и работает с любым профилем.
#
# 3. ВЕРДИКТ. По окончании сверяет вехи с образцовым логом профиля, если он есть. Иначе цифры
#    прогона читать глазами, а «дошло или нет» — вопрос, на который должен отвечать инструмент.
#
# Использование:
#   scripts/mr-launch.sh                 — выбрать профиль из списка
#   scripts/mr-launch.sh <профиль> [сек] — запустить сразу
#   scripts/mr-launch.sh --list          — только показать профили
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
PROFILES="$ROOT/profiles"
STATE="$ROOT/build/.последний-профиль"
SECS_DEFAULT=1800

esc=$'\033'; B="${esc}[1m"; N="${esc}[0m"; G="${esc}[32m"; R="${esc}[31m"; Y="${esc}[33m"

list_profiles() {
  find "$PROFILES" -maxdepth 2 -name 'final-child.json' 2>/dev/null |
    sed "s|$PROFILES/||; s|/final-child.json||" | sort
}

if [ "${1:-}" = "--list" ]; then list_profiles; exit 0; fi

name="${1:-}"
secs="${2:-$SECS_DEFAULT}"

# ── Выбор профиля ─────────────────────────────────────────────────────────────────────────────
if [ -z "$name" ]; then
  mapfile -t items < <(list_profiles) 2>/dev/null || { items=(); while IFS= read -r l; do items+=("$l"); done < <(list_profiles); }
  [ "${#items[@]}" -gt 0 ] || { echo "${R}В $PROFILES нет ни одного профиля (нужен final-child.json).${N}"; exit 1; }
  last=""; [ -f "$STATE" ] && last="$(cat "$STATE")"
  echo "${B}Профили MacRunner${N}"
  i=1; for p in "${items[@]}"; do
    mark=" "; [ "$p" = "$last" ] && mark="${G}•${N}"
    printf "  %s %2d) %s\n" "$mark" "$i" "$p"; i=$((i+1))
  done
  [ -n "$last" ] && echo "  ${G}•${N} — запускали прошлый раз"
  printf "Номер (Enter — %s): " "${last:-1}"
  read -r ans
  if [ -z "$ans" ]; then
    name="${last:-${items[0]}}"
  elif [ "$ans" -ge 1 ] 2>/dev/null && [ "$ans" -le "${#items[@]}" ]; then
    name="${items[$((ans-1))]}"
  else
    echo "${R}Нет такого номера.${N}"; exit 1
  fi
fi

P="$PROFILES/$name"
[ -f "$P/final-child.json" ] || { echo "${R}Профиля '$name' нет в $PROFILES.${N}"; exit 1; }
mkdir -p "$(dirname "$STATE")" && printf '%s' "$name" > "$STATE"

# ── Выбор диста: свой или этаж ────────────────────────────────────────────────────────────────
# Профиль даёт ПЕРЕМЕННЫЕ, дист даёт ДВОИЧНЫЕ ФАЙЛЫ, и это независимые вещи. Именно так 07.08
# проверялся floor26: переменные профиля «эталон-меню», носители этажа. Без такой развязки
# сравнить две сборки на одинаковом окружении нельзя.
DIST=""
if [ -z "${MR_DIST:-}" ]; then
  floors=()
  for st in "/Volumes/MacOS 1/MacRunner-ARM64EC-floors" "/Users/timurtoby/Documents/MacRunner"; do
    [ -d "$st" ] || continue
    for f in "$st"/*/; do
      for l in "" "dist/" "dist-arm64ec-spike/" "payload/dist/"; do
        [ -f "$f$l/lib/wine/aarch64-unix/ntdll.so" ] && { floors+=("${f%/}|$l"); break; }
      done
    done
  done
  echo
  echo "${B}Откуда брать двоичные файлы${N}"
  echo "     0) свой дист профиля  ${G}(обычный выбор)${N}"
  i=1; for fl in "${floors[@]}"; do
    printf '    %2d) %s\n' "$i" "$(basename "${fl%%|*}")"; i=$((i+1))
  done
  printf "Номер (Enter — 0): "
  read -r da
  if [ -n "$da" ] && [ "$da" -ge 1 ] 2>/dev/null && [ "$da" -le "${#floors[@]}" ]; then
    fl="${floors[$((da-1))]}"; DIST="${fl%%|*}/${fl##*|}"; DIST="${DIST%/}"
  fi
else
  DIST="$MR_DIST"
fi

echo
echo "${B}═══ $name ═══${N}"
[ -f "$P/О-ПРОФИЛЕ.txt" ] && head -6 "$P/О-ПРОФИЛЕ.txt"

# ── Карантин ──────────────────────────────────────────────────────────────────────────────────
# Карантин снимаем и с диста, если он отдельный: этаж тоже мог приехать из архива.
for TGT in "$P" ${DIST:+"$DIST"}; do
q=$(xattr -r -p com.apple.quarantine "$TGT" 2>/dev/null | wc -l | tr -d ' ')
if [ "${q:-0}" != 0 ]; then
  echo "${Y}карантин macOS: $q меток — снимаю (содержимое файлов не меняется)${N}"
  xattr -srd com.apple.quarantine "$TGT" 2>/dev/null
  q2=$(xattr -r -p com.apple.quarantine "$TGT" 2>/dev/null | wc -l | tr -d ' ')
  if [ "${q2:-0}" != 0 ]; then
    echo "${R}осталось $q2 — Gatekeeper может заблокировать загрузку${N}"
  else
    echo "${G}карантин снят${N}"
  fi
else
  echo "карантин ($(basename "$TGT")): чисто"
fi
done

[ -n "$DIST" ] && echo "${Y}дист: $(basename "$DIST")${N}" || echo "дист: свой, из профиля"
echo "бюджет: ${secs}с"
echo "Запускаю. Окно можно свернуть, закрывать нельзя."
echo

# ── Прогон ────────────────────────────────────────────────────────────────────────────────────
# ПРИБОРЫ КАДРОВ — ТОЛЬКО ПО ЯВНОЙ ПРОСЬБЕ: MR_FRAMES=1.
# Сначала я включил их по умолчанию, рассудив, что выключенный прибор стоил целого дня неверных
# выводов (Present1 печатается лишь при MACRUNNER_DXMT_SWAPCHAIN_TRACE, и тот был выключен, пока
# кадры шли). Замер это опроверг: прибор не наблюдает, а ЛОМАЕТ.
# Один и тот же floor26, один профиль, разница только в этих двух переменных:
#     без приборов   20 480 отказов, темп 860/с,     занятость 0%,  все вехи за 615 с
#     с приборами   125 034 496,     темп 116 435/с, занятость 23%, встал после 5 кадров
# Шесть тысяч раз. Считывание кадра тащит поверхность обратно в память, и обход гостевых
# страниц сам порождает шторм — тот, что мы утром чинили с другого конца.
# Поэтому: обычный прогон идёт БЕЗ них, а кадры смотрят отдельным прогоном, зная цену.
FRAME_ENV=()
if [ "${MR_FRAMES:-0}" = 1 ]; then
  echo "${Y}приборы кадров ВКЛЮЧЕНЫ — прогон будет медленнее и может встать после первых кадров${N}"
  FRAME_ENV=(MACRUNNER_DXMT_FRAME_DUMP=1 MACRUNNER_DXMT_SWAPCHAIN_TRACE=1)
fi
"$ROOT/scripts/mr-profile.sh" run "$name" "$secs" ${DIST:+"$DIST"} "${FRAME_ENV[@]}" &
RUNPID=$!

# Бегущая строка. Рундир ищем ТОЛЬКО новее отметки времени старта: 07.08 сторож дважды хватал
# рундир прошлого прогона и объявлял его концом текущего.
STARTED=$(date +%s)
(
  D=""
  while kill -0 $RUNPID 2>/dev/null; do
    if [ -z "$D" ]; then
      for d in $(ls -dt "$ROOT"/reports/phase4-hollow-knight/laneA-профиль-* 2>/dev/null); do
        [ "$(stat -f '%m' "$d" 2>/dev/null || echo 0)" -ge "$STARTED" ] && { D="$d"; break; }
      done
    fi
    if [ -n "$D" ] && [ -f "$D/run.log" ]; then
      printf "\r  строк:%-8s движок:%s устр:%s объекты:%s языки:%s swapchain:%s КАДРОВ:%s искл:%s ввод:%-4s " \
        "$(wc -l < "$D/run.log" | tr -d ' ')" \
        "$(grep -ac 'Initialize engine version' "$D/run.log")" \
        "$(grep -ac 'GfxDevice: creating' "$D/run.log")" \
        "$(grep -ac 'Loaded Objects' "$D/run.log")" \
        "$(grep -ac 'Discovered supported languages' "$D/run.log")" \
        "$(grep -ac 'CreateSwapChain' "$D/run.log")" \
        "$(grep -o 'dxmt-frame-dump: frame=[0-9]*' "$D/run.log" | sed 's/.*=//' | tail -1 | grep -E '^[0-9]+$' || echo 0)" \
        "$(grep -ac 'stage-exception_throw' "$D/run.log")" \
        "$(grep -ac 'macrunner-ui-input' "$D/run.log")"
    else
      printf "\r  готовлю прогон (преполётные проверки)… "
    fi
    sleep 3
  done
) & TICKER=$!

wait $RUNPID; rc=$?
kill $TICKER 2>/dev/null
echo; echo

# ── Вердикт ───────────────────────────────────────────────────────────────────────────────────
D=""
for d in $(ls -dt "$ROOT"/reports/phase4-hollow-knight/laneA-профиль-* 2>/dev/null); do
  [ "$(stat -f '%m' "$d" 2>/dev/null || echo 0)" -ge "$STARTED" ] && { D="$d"; break; }
done

if [ -z "$D" ] || [ ! -f "$D/run.log" ]; then
  echo "${R}Прогон не начался.${N} Причина — в выводе выше (преполётная проверка отказала)."
  echo "Нажми Enter, чтобы закрыть."; read -r _; exit 1
fi

echo "${B}Рундир:${N} $(basename "$D")"
echo "${B}Длительность:${N} $(grep -oE '\+[0-9]+\.[0-9]+s' "$D/run.log" | tail -1)   выход: $(grep -oE '\[mr-run\] exit=[0-9]+' "$D/run.log" | tail -1 | sed 's/.*exit=//')"
echo

OB="$P/образец-run.log"
if [ -f "$OB" ]; then
  echo "${B}Вехи против образцового лога профиля:${N}"
  while IFS='|' read -r label marker; do
    a=$(grep -ac "$marker" "$OB"); b=$(grep -ac "$marker" "$D/run.log")
    if [ "$b" -ge "$a" ] && [ "$a" -gt 0 ]; then s="${G}взята${N}"
    elif [ "$a" -eq 0 ]; then s="—  (образец её тоже не давал)"
    else s="${R}НЕТ${N}"; fi
    printf "  %-26s образец=%-4s сейчас=%-4s %b\n" "$label" "$a" "$b" "$s"
  done <<'МЕТКИ'
движок|Initialize engine version
устройство|GfxDevice: creating
объекты|Loaded Objects now
языки|Discovered supported languages
язык восстановлен|Restored language
swapchain|CreateSwapChain
кадр (Present1)|Present1
МЕТКИ
else
  echo "У профиля нет образец-run.log — сравнивать не с чем, вот сырые вехи:"
  for m in 'Initialize engine version' 'GfxDevice: creating' 'Loaded Objects now' \
           'Discovered supported languages' 'CreateSwapChain' 'Present1'; do
    printf "  %-34s %s\n" "$m" "$(grep -ac "$m" "$D/run.log")"
  done
fi

echo
if [ "$rc" = 0 ]; then echo "${G}прогон завершён штатно${N}"; else echo "${Y}код возврата $rc — смотри вывод выше${N}"; fi
echo "Нажми Enter, чтобы закрыть."
read -r _
