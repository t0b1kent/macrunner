#!/usr/bin/env bash
# mr-profile.sh — сохранить рабочую конфигурацию прогона и запускать её одной командой.
#
# ── Зачем это ─────────────────────────────────────────────────────────────────────────────────
# 05.08.2026 за один день трижды повторилась одна и та же потеря: рабочее состояние получено
# (кадр, меню, курсор), а через несколько часов воспроизвести его нечем. Причина не в том, что
# что-то стёрли — а в том, что конфигурация прогона живёт в 60+ переменных окружения, и собранная
# «по памяти» она теряет половину. В тот день из 67 переменных эталона было потеряно 43, включая
# MACRUNNER_PREFIX_SYSTEM32_ARCH и WINEDLLOVERRIDES — после чего DXMT перестал отдавать адаптер,
# и полдня ушло на диагностику дефекта, которого не существовало.
#
# Профиль — это снимок ВСЕГО: переменные из final-child.json, путь и отпечаток диста, корень
# графики и достигнутые вехи. Работает курсор — сохрани профиль. Нужно вернуться — запусти его
# по имени. Ничего собирать руками не надо.
#
# ── Как пользоваться ──────────────────────────────────────────────────────────────────────────
#   scripts/mr-profile.sh save <имя> <папка-прогона>     сохранить конфигурацию прогона
#   scripts/mr-profile.sh list                           показать профили и что каждый достиг
#   scripts/mr-profile.sh show <имя>                     подробности профиля
#   scripts/mr-profile.sh run  <имя> [секунды] [DIST]    запустить (через preflight-run.sh)
#
# DIST позволяет подменить ТОЛЬКО дист, оставив остальное эталонным — так проверяют, что даёт
# новая сборка при прочих равных. Без него берётся дист, записанный в профиле.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
DIR="$ROOT/profiles"
mkdir -p "$DIR"

die() { echo "mr-profile: $*" >&2; exit 1; }

# Вехи, по которым видно, докуда дошёл прогон. Порядок — по ходу запуска игры.
MILESTONES=(
  "Initialize engine version|движок"
  "GfxDevice: creating|устройство"
  "failed to create device|ОТКАЗ-устройства"
  "Loaded Objects|объекты"
  "cannot find the FreeType|ШРИФТОВ-НЕТ"
  "stage-exception_throw|исключения"
  # Ниже — то, что БЫЛО в журнале 28.07, когда меню стояло на экране (снимок вехи
  # checkpoints/20260728-MAIN-MENU-REACHED/evidence/run.log). Это и есть признаки того,
  # что управляемый код игры пошёл: свои MonoBehaviour, язык, путь отказа GOG, старт уровня.
  "Discovered supported languages|язык"
  "GOG was not initialised|GOG-отказ-пройден"
  "Performing automatic level start|старт-уровня"
  "macrunner-ui-input|ввод"
)

scan_milestones() {  # $1 = run.log
  local log="$1" out=""
  [ -f "$log" ] || { echo "нет журнала"; return; }
  for m in "${MILESTONES[@]}"; do
    local pat="${m%%|*}" name="${m##*|}"
    local n; n=$(grep -ac -- "$pat" "$log" 2>/dev/null); n="${n//[^0-9]/}"; n="${n:-0}"
    [ "$n" != "0" ] && out="$out $name=$n"
  done
  [ -n "$out" ] && echo "${out# }" || echo "вех нет"
}

cmd_save() {
  local name="${1:?нужно имя}" run="${2:?нужна папка прогона}"
  [ -f "$run/final-child.json" ] || die "в $run нет final-child.json — этот прогон шёл мимо оснастки, сохранять нечего"
  local p="$DIR/$name"
  # КОПИЯ ПЕРЕД ПЕРЕЗАПИСЬЮ — главное правило проекта
  [ -d "$p" ] && mv "$p" "$p.заменён-$(date +%Y%m%d-%H%M%S)"
  mkdir -p "$p"
  cp -p "$run/final-child.json" "$p/final-child.json"

  local dist; dist=$(python3 - "$run/final-child.json" <<'PY'
import json,sys
e={x['name']:x.get('value','') for x in json.load(open(sys.argv[1]))['environment']['entries']}
print(e.get("MACRUNNER_WINE_DIST","") or e.get("MACRUNNER_LANEA_WINE_DIST",""))
PY
)
  {
    echo "профиль: $name"
    echo "сохранён: $(date '+%Y-%m-%d %H:%M:%S')"
    echo "источник: $run"
    echo "дист:     ${dist:-неизвестен}"
    if [ -f "$dist/lib/wine/aarch64-unix/ntdll.so" ]; then
      echo "ntdll.so: $(shasum -a256 "$dist/lib/wine/aarch64-unix/ntdll.so" | cut -c1-16)"
      echo "выравнивание секций: $(python3 - "$dist" <<'PY'
import struct,sys,glob
g=glob.glob(sys.argv[1]+"/lib/wine/aarch64-windows/ntdll.dll")
if g:
    d=open(g[0],'rb').read(0x400); e=struct.unpack_from('<I',d,0x3c)[0]
    print(struct.unpack_from('<I',d,e+0x38)[0])
else: print("?")
PY
)"
    fi
    echo "вехи:     $(scan_milestones "$run/run.log")"
  } > "$p/О-ПРОФИЛЕ.txt"

  # ── ВСЁ В ОДНОМ МЕСТЕ ─────────────────────────────────────────────────────────────────────
  # Профиль, хранящий только ПУТЬ к дисту, бесполезен: дист пересоберут — и профиль мёртв.
  # Ровно это случилось 05.08. Поэтому кладём внутрь сам дист, графику и шаблон префикса.
  # На APFS `cp -c` — клонирование: мгновенно и почти без расхода места, пока файлы не изменят.
  local CP="cp -Rc"; cp -c /dev/null /dev/null 2>/dev/null || CP="cp -R"
  if [ -n "$dist" ] && [ -d "$dist" ]; then
    echo "  клонирую дист…"
    $CP "$dist" "$p/dist" 2>/dev/null || cp -R "$dist" "$p/dist"
    # Шрифты: wine зовёт freetype через dlopen по голому имени, а в Homebrew dyld не смотрит.
    # Без этого — «Wine cannot find the FreeType font library» и ноль TrueType-шрифтов.
    if ! "$ROOT/scripts/mr-fonts.sh" --check "$p/dist" >/dev/null 2>&1; then
      echo "  докладываю шрифтовые библиотеки в дист профиля…"
      "$ROOT/scripts/mr-fonts.sh" "$p/dist" >/dev/null 2>&1 || echo "  ⚠️  шрифты положить не удалось"
    fi
  fi
  local gfx; gfx=$(python3 - "$run/final-child.json" <<'PY'
import json,sys
e={x['name']:x.get('value','') for x in json.load(open(sys.argv[1]))['environment']['entries']}
print(e.get("MACRUNNER_DXMT_ROOT",""))
PY
)
  [ -d "$gfx" ] && { echo "  клонирую графику…"; $CP "$gfx" "$p/graphics" 2>/dev/null || cp -R "$gfx" "$p/graphics"; }
  local tpl; tpl=$(python3 - "$run/final-child.json" <<'PY'
import json,sys
e={x['name']:x.get('value','') for x in json.load(open(sys.argv[1]))['environment']['entries']}
print(e.get("MACRUNNER_MR_RUN_PREFIX_TEMPLATE",""))
PY
)
  [ -d "$tpl" ] && { echo "  клонирую шаблон префикса…"; $CP "$tpl" "$p/prefix-template" 2>/dev/null || cp -R "$tpl" "$p/prefix-template"; }
  # журнал того прогона — доказательство, что профиль действительно это умел
  [ -f "$run/run.log" ] && $CP "$run/run.log" "$p/образец-run.log" 2>/dev/null

  echo "содержимое: dist=$([ -d "$p/dist" ] && echo да || echo НЕТ) graphics=$([ -d "$p/graphics" ] && echo да || echo нет) prefix=$([ -d "$p/prefix-template" ] && echo да || echo нет)" >> "$p/О-ПРОФИЛЕ.txt"
  echo "размер:     $(du -sh "$p" 2>/dev/null | cut -f1)" >> "$p/О-ПРОФИЛЕ.txt"
  echo "✓ профиль сохранён: $p"
  sed 's/^/    /' "$p/О-ПРОФИЛЕ.txt"
}

cmd_list() {
  local n=0
  for p in "$DIR"/*/; do
    [ -d "$p" ] || continue
    [ -f "$p/О-ПРОФИЛЕ.txt" ] || continue
    n=$((n+1))
    printf "  %-26s %s\n" "$(basename "$p")" "$(grep '^вехи:' "$p/О-ПРОФИЛЕ.txt" | cut -d: -f2-)"
  done
  [ "$n" = 0 ] && echo "  профилей пока нет — сохрани первый: scripts/mr-profile.sh save <имя> <папка-прогона>"
  return 0
}

# Обновить описание профиля по его СОБСТВЕННОМУ дисту.
#
# Зачем. 06.08 двоичные файлы в profiles/<имя>/dist обновлялись несколько раз (выравнивание
# секций 16 КБ, атомарные LSE, приборы), а О-ПРОФИЛЕ.txt остался от первого сохранения:
# старый отпечаток ntdll.so и вехи чужого прогона. Профиль, который описывает не себя,
# вводит в заблуждение ровно там, где должен давать опору.
cmd_refresh() {
  local name="${1:?нужно имя}"
  local p="$DIR/$name"
  [ -d "$p" ] || die "профиля '$name' нет"
  local d="$p/dist"
  {
    echo "профиль: $name"
    echo "обновлён: $(date '+%Y-%m-%d %H:%M:%S')"
    [ -f "$p/ИСТОЧНИК.txt" ] && echo "источник: $(cat "$p/ИСТОЧНИК.txt")"
    echo "дист:     собственный ($d)"
    if [ -f "$d/lib/wine/aarch64-unix/ntdll.so" ]; then
      echo "ntdll.so: $(shasum -a256 "$d/lib/wine/aarch64-unix/ntdll.so" | cut -c1-16)  $(date -r "$d/lib/wine/aarch64-unix/ntdll.so" '+%m-%d %H:%M')"
      echo "выравнивание секций: $(python3 - "$d" <<'PY2'
import struct,sys,glob
g=glob.glob(sys.argv[1]+"/lib/wine/aarch64-windows/ntdll.dll")
if g:
    x=open(g[0],'rb').read(0x400); e=struct.unpack_from('<I',x,0x3c)[0]
    print(struct.unpack_from('<I',x,e+0x38)[0])
else: print("?")
PY2
)"
      echo "правки в двоичных:"
      for m in ntdll xtajit xtajit64; do
        f="$d/lib/wine/aarch64-unix/$m.so"; [ -f "$f" ] || continue
        printf "  %-10s LSE=%s приборы=%s\n" "$m" \
          "$(strings -a "$f" 2>/dev/null | grep -c MACRUNNER_HB_LSE_ATOMICS)" \
          "$(strings -a "$f" 2>/dev/null | grep -c 'alloc-site')"
      done
    fi
    echo "содержимое: dist=$([ -d "$d" ] && echo да || echo НЕТ) graphics=$([ -d "$p/graphics" ] && echo да || echo нет) prefix=$([ -d "$p/prefix-template" ] && echo да || echo нет)"
    echo "размер:     $(du -sh "$p" 2>/dev/null | cut -f1)"
    [ -f "$p/ДОП-ПЕРЕМЕННЫЕ.txt" ] && { echo "переключатели профиля:"; sed 's/^/  /' "$p/ДОП-ПЕРЕМЕННЫЕ.txt"; }
    [ -f "$p/ВЕХИ.txt" ] && { echo "последние вехи: $(cat "$p/ВЕХИ.txt")"; }
  } > "$p/О-ПРОФИЛЕ.txt"
  echo "✓ описание обновлено"
  sed 's/^/    /' "$p/О-ПРОФИЛЕ.txt"
}

# Записать переключатели В САМ ПРОФИЛЬ, чтобы их не надо было передавать каждый раз.
cmd_gates() {
  local name="${1:?нужно имя}"; shift
  local p="$DIR/$name"
  [ -d "$p" ] || die "профиля '$name' нет"
  printf '%s\n' "$@" > "$p/ДОП-ПЕРЕМЕННЫЕ.txt"
  echo "✓ переключатели профиля записаны ($#):"
  sed 's/^/    /' "$p/ДОП-ПЕРЕМЕННЫЕ.txt"
}

cmd_show() {
  local p="$DIR/${1:?нужно имя}"
  [ -d "$p" ] || die "профиля '$1' нет"
  cat "$p/О-ПРОФИЛЕ.txt"
}

cmd_run() {
  local name="${1:?нужно имя}" secs="${2:-5400}"
  shift 2 2>/dev/null || shift $#
  # Третий аргумент может быть ЛИБО путём к дисту, ЛИБО уже переменной. Различаем по знаку "=":
  # без этого MACRUNNER_WINEMAC_GAROOT_FALLBACK=1 молча уезжал в подстановку диста и терялся.
  local dist=""
  if [ $# -gt 0 ] && [ "${1#*=}" = "$1" ]; then dist="$1"; shift; fi
  local -a EXTRA=(); [ $# -gt 0 ] && EXTRA=("$@")
  local p="$DIR/$name"
  [ -f "$p/final-child.json" ] || die "профиля '$name' нет"

  # Дист берём ИЗ ПРОФИЛЯ, если он там есть — тогда профиль самодостаточен и не зависит от
  # того, что за день сделали с engine/wine/dist. Аргумент командной строки перекрывает.
  if [ -z "$dist" ] && [ -d "$p/dist" ]; then dist="$p/dist"; fi

  # Шрифты чиним на месте: старые профили сохранялись без них, а это три жалобы wine за
  # прогон и полное отсутствие TrueType. Дешевле доложить, чем пересохранять профиль.
  if [ -n "$dist" ] && [ -d "$dist/lib/wine/aarch64-unix" ] \
     && ! "$ROOT/scripts/mr-fonts.sh" --check "$dist" >/dev/null 2>&1; then
    echo "  шрифтов в дисте нет — докладываю…"
    "$ROOT/scripts/mr-fonts.sh" "$dist" >/dev/null 2>&1 \
      && echo "  ✓ freetype + fontconfig на месте" \
      || echo "  ⚠️  не удалось; будет «cannot find the FreeType font library»"
  fi

  local args; args=$(python3 - "$p/final-child.json" "$ROOT" "$dist" "$p" <<'PY'
import json,sys,os
env={e['name']:e.get('value','') for e in json.load(open(sys.argv[1]))['environment']['entries']}
ROOT, override = sys.argv[2], (sys.argv[3] if len(sys.argv)>3 else "")
PROF = sys.argv[4] if len(sys.argv)>4 else ""
# графика и шаблон префикса тоже из профиля, иначе «всё в одном месте» не работает
if PROF and os.path.isdir(os.path.join(PROF,"graphics")):
    env["MACRUNNER_DXMT_ROOT"]=os.path.join(PROF,"graphics")
if PROF and os.path.isdir(os.path.join(PROF,"prefix-template")):
    env["MACRUNNER_MR_RUN_PREFIX_TEMPLATE"]=os.path.join(PROF,"prefix-template")
# ДИСТОВ ДВА, И ОБА ОБЯЗАТЕЛЬНЫ. Запуск идёт из dist-arm64ec-spike/bin/wine, а ntdll.so
# грузится из MACRUNNER_WINE_DIST — это РАЗНЫЕ деревья. 06.08 я полдня разворачивал правки
# только в первое и мерил файл, которого в процессе не было: DFSC не печатался, гейт не
# срабатывал, и оба замера оказались недействительны. Профиль обязан нести оба.
if PROF and os.path.isdir(os.path.join(PROF,"wine-dist")):
    wd=os.path.join(PROF,"wine-dist")
    env["MACRUNNER_WINE_DIST"]=wd
    # 07.08: ЭТОГО ИМЕНИ ЗАПУСКАТЕЛЬ НЕ ЧИТАЕТ. Дист выбирает laneA-run-hk.sh:28 —
    #     WINE_DIST="${MACRUNNER_LANEA_WINE_DIST:-$ROOT/engine/wine/dist-arm64ec-spike}"
    # то есть по ДРУГОМУ имени, с молчаливым откатом на рабочее дерево. MACRUNNER_WINE_DIST
    # читают только сборочные скрипты (build-dxmt-tests.sh, milestone-dist-snapshot.sh), в
    # цепочку запуска игры они не входят.
    # Следствие: wine-dist профиля не использовался НИ РАЗУ. Все 12 прогонов 07.08 грузили
    # ntdll.so из engine/wine/dist-arm64ec-spike, и в логах ноль обращений к папке профиля.
    # Оператор это заметил раньше меня — «значит что-то подменяется ещё где-то». Подменялось.
    env["MACRUNNER_LANEA_WINE_DIST"]=wd
    if os.path.exists(os.path.join(wd,"bin","wine")):
        env["MACRUNNER_WINE_BIN"]=os.path.join(wd,"bin","wine")
# Привязана к хешу бинаря: при смене диста подсовывает чужие трансляции и даёт exit=53.
DROP = {"MACRUNNER_HB_TRANSLATION_CACHE_ROOT"}
# Каталоги самого прогона — их создаёт новый прогон, переносить нельзя.
DROP |= {"MACRUNNER_RUN_DIR","WINEPREFIX","MACRUNNER_FLIGHT_PATH",
         "MACRUNNER_FLIGHT_RECORDER_FILE","MACRUNNER_FLIGHT_RECORDER_PATH","DXMT_LOG_PATH"}
out=[]
for k,v in sorted(env.items()):
    if k in DROP or not k.startswith(("MACRUNNER_","WINE","DXMT")): continue
    if override and k in ("MACRUNNER_WINE_DIST","MACRUNNER_LANEA_WINE_DIST"): v=override
    out.append("%s=%s" % (k,v))
print("\n".join(out))
PY
)
  [ -n "$args" ] || die "не удалось собрать набор переменных"

  # Гейты, которых в дереве больше НЕТ. Архивный профиль их выставляет, а код давно не читает —
  # значит они инертны, и требовать их наличия в бинаре бессмысленно. Но молчать нельзя: если
  # среди них окажется что-то важное (05.08 так вскрылось, что MACRUNNER_HB_START_GAME_ACTUATOR
  # исчез из кода, хотя в памяти он числился лекарством), это надо увидеть.
  # Один проход по исходникам: список всех имён гейтов, которые код действительно читает.
  local KNOWN; KNOWN="$(grep -rhoE 'MACRUNNER_[A-Z0-9_]+' \
      engine/hyperbridge/src engine/hyperbridge/include engine/wine/dlls engine/wine/programs scripts tools \
      --include='*.c' --include='*.h' --include='*.sh' --include='*.in' --include='*.py' \
      --exclude='mr-profile.sh' --exclude='preflight-run.sh' 2>/dev/null | sort -u)"
  local -a ENVS=() DEAD=()
  while IFS= read -r line; do
    [ -n "$line" ] || continue
    local nm="${line%%=*}"
    case "$nm" in
      MACRUNNER_*)
        # по КЭШУ имён (см. ниже), а не рекурсивным grep по 23 ГБ — иначе запуск висит минутами
        if ! printf '%s\n' "$KNOWN" | grep -qx -- "$nm"; then DEAD+=("$nm"); continue; fi ;;
    esac
    ENVS+=("$line")
  done <<< "$args"
  if [ "${#DEAD[@]}" -gt 0 ]; then
    echo "  ⚠️  в коде БОЛЬШЕ НЕТ (выброшены из набора): ${DEAD[*]}"
  fi
  # Дополнительные переменные поверх профиля — так включают то, чего в эталоне не было.
  # Например MACRUNNER_WINEMAC_GAROOT_FALLBACK=1: без него окно создаётся на стороне Wine,
  # но metal-вью не припарентен и на экране НИЧЕГО не видно (правка от 27.07, гейт по
  # умолчанию выключен). Эталонный прогон доходил до меню вслепую — по журналу, не глазами.
  # Переключатели, записанные В САМОМ ПРОФИЛЕ, применяются всегда; аргументы командной
  # строки идут после и перекрывают их. Так рабочий набор не теряется при запуске «на память».
  if [ -f "$p/ДОП-ПЕРЕМЕННЫЕ.txt" ]; then
    local -a FROMFILE=()
    while IFS= read -r ln; do [ -n "$ln" ] && FROMFILE+=("$ln"); done < "$p/ДОП-ПЕРЕМЕННЫЕ.txt"
    [ "${#FROMFILE[@]}" -gt 0 ] && { echo "  переключатели профиля: ${FROMFILE[*]}"; EXTRA=("${FROMFILE[@]}" ${EXTRA[@]+"${EXTRA[@]}"}); }
  fi
  # Схлопываем повторы по ИМЕНИ, побеждает последнее. Иначе в «итоговых» одна переменная
  # печатается дважды с разными значениями, и по строке не видно, что реально уехало —
  # ровно так 06.08 я не мог сказать, выключился актуатор или нет.
  if [ "${#EXTRA[@]-0}" -gt 0 ]; then
    local -a DEDUP=(); local i j nm keep
    for (( i=${#EXTRA[@]}-1; i>=0; i-- )); do
      nm="${EXTRA[$i]%%=*}"; keep=1
      for e in ${DEDUP[@]+"${DEDUP[@]}"}; do [ "${e%%=*}" = "$nm" ] && { keep=0; break; }; done
      [ "$keep" = 1 ] && DEDUP=("${EXTRA[$i]}" ${DEDUP[@]+"${DEDUP[@]}"})
    done
    EXTRA=(${DEDUP[@]+"${DEDUP[@]}"})
  fi
  if [ "${#EXTRA[@]-0}" -gt 0 ]; then
    echo "  + итоговые: ${EXTRA[*]-}"
    for kv in ${EXTRA[@]+"${EXTRA[@]}"}; do
      local en="${kv%%=*}"
      local -a FILT=(); for e in ${ENVS[@]+"${ENVS[@]}"}; do [ "${e%%=*}" = "$en" ] || FILT+=("$e"); done
      ENVS=(${FILT[@]+"${FILT[@]}"} "$kv")
    done
  fi
  echo "  профиль '$name': ${#ENVS[@]} переменных, ${secs}с${dist:+, дист подменён на $dist}"
  exec scripts/preflight-run.sh "профиль-$name" "$secs" \
       'Initialize engine version,macrunner-hb-d3d-boundary' ${ENVS[@]+"${ENVS[@]}"}
}

# Ярлык на рабочем столе: двойной клик — и прогон пошёл, как обычная игра.
# Именно этого не хватало: рабочее состояние было, а вернуться к нему можно было только
# через сборку 60 переменных руками. Теперь — клик.
cmd_desktop() {
  local name="${1:?нужно имя}" secs="${2:-5400}"
  shift 2 2>/dev/null || shift $#
  local extra="$*"
  local p="$DIR/$name"
  [ -f "$p/final-child.json" ] || die "профиля '$name' нет"
  local out="$HOME/Desktop/▶ HK — $name.command"
  cat > "$out" <<EOF
#!/bin/bash
# Запуск сохранённой конфигурации MacRunner. Двойной клик в Finder.
cd "$ROOT" || exit 1
clear
echo "═══════════════════════════════════════════════"
echo "  Hollow Knight — профиль: $name"
echo "═══════════════════════════════════════════════"
cat "$p/О-ПРОФИЛЕ.txt"
echo "───────────────────────────────────────────────"
echo "Запускаю. Окно можно свернуть, закрывать нельзя."
echo
scripts/mr-profile.sh run "$name" $secs $extra &
RUNPID=\$!
# Показываем вехи по мере прохождения, иначе окно молчит и непонятно, живо ли оно.
( while kill -0 \$RUNPID 2>/dev/null; do
    D=\$(ls -dt reports/phase4-hollow-knight/laneA-профиль-* 2>/dev/null | head -1)
    if [ -n "\$D" ] && [ -f "\$D/run.log" ]; then
      printf "\\r  строк:%-7s движок:%s язык:%s объекты:%-6s уровень:%s искл:%s ввод:%s  " \\
        "\$(wc -l < "\$D/run.log" | tr -d ' ')" \\
        "\$(grep -ac 'Initialize engine version' "\$D/run.log")" \\
        "\$(grep -ac 'Discovered supported languages' "\$D/run.log")" \\
        "\$(grep -ao 'Loaded Objects now: [0-9]*' "\$D/run.log" | sed 's/.*: //' | tail -1)" \\
        "\$(grep -ac 'Performing automatic level start' "\$D/run.log")" \\
        "\$(grep -ac 'stage-exception_throw' "\$D/run.log")" \\
        "\$(grep -ac 'macrunner-ui-input' "\$D/run.log")"
    fi
    sleep 3
  done ) &
TICKER=\$!
wait \$RUNPID; rc=\$?
kill \$TICKER 2>/dev/null; echo
echo
if [ \$rc = 0 ]; then echo "✅ прогон завершён штатно"; else echo "⚠️  код возврата \$rc — смотри вывод выше"; fi
echo "Нажми Enter, чтобы закрыть."
read -r _
EOF
  chmod +x "$out"
  echo "✓ ярлык создан: $out"
  echo "  Открой Finder → Рабочий стол → двойной клик."
}

case "${1:-}" in
  save)    shift; cmd_save    "$@" ;;
  refresh) shift; cmd_refresh "$@" ;;
  gates)   shift; cmd_gates   "$@" ;;
  list)    shift; cmd_list    "$@" ;;
  show)    shift; cmd_show    "$@" ;;
  run)     shift; cmd_run     "$@" ;;
  desktop) shift; cmd_desktop "$@" ;;
  *) sed -n '1,32p' "$0" | grep '^#' | sed 's/^# \{0,1\}//' ;;
esac
