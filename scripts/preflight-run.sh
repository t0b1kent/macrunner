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
# 2026-08-05: область поиска расширена. Раньше искали ТОЛЬКО в engine/hyperbridge/src, поэтому
# полный эталонный набор (60 переменных из final-child.json рабочего прогона) преflight отвергал
# целиком: MACRUNNER_GRAPHICS_BACKEND, MACRUNNER_PREFIX_SYSTEM32_ARCH и прочие живут в исходниках
# Wine и в самих скриптах. Проверка обязана покрывать все места, где гейты читаются, иначе она
# запрещает ровно тот прогон, ради воспроизводимости которого написана.
# ВАЖНО про скорость: искать по всей папке engine НЕЛЬЗЯ — там лежат дисты и артефакты на
# десятки гигабайт, и 60 проходов grep вешают запуск на минуты (поймано 05.08: окно ярлыка
# стояло молча, в заголовке висел grep). Ищем ОДИН раз, только по исходникам, и складываем
# все встреченные имена в множество.
KNOWN_GATES="$(
  { grep -rhoE 'MACRUNNER_[A-Z0-9_]+' \
      engine/hyperbridge/src engine/hyperbridge/include \
      engine/wine/dlls engine/wine/programs scripts tools \
      --include='*.c' --include='*.h' --include='*.sh' --include='*.in' --include='*.py' \
      --exclude='mr-profile.sh' --exclude='preflight-run.sh' 2>/dev/null
  } | sort -u )"
for kv in "${ENVS[@]}"; do
  name="${kv%%=*}"
  case "$name" in MACRUNNER_*) ;; *) continue;; esac
  if ! printf '%s\n' "$KNOWN_GATES" | grep -qx -- "$name"; then
    note "FAIL: гейт $name не встречается в исходниках — опечатка или имя, выброшенное из кода"
    fail=1
  fi
done

# ── 2. The gate must be present in the DEPLOYED binary, not just in the tree. ─────────────────
# A publish step can die silently and leave the old .so in place; the tree then proves nothing.
# 2026-08-05: проверяем только гейты ДВИЖКА (те, что читает наш код в xtajit64.so). Переменные
# оснастки — пути, корни, режим графики — в бинарь не попадают по определению, и требовать их
# там значило запрещать любой полный набор.
# Дист берём из набора, а не из жёсткой строки: профиль может указывать на любой дист.
PF_DIST=""
for kv in "${ENVS[@]}"; do
  case "${kv%%=*}" in MACRUNNER_WINE_DIST|MACRUNNER_LANEA_WINE_DIST) PF_DIST="${kv#*=}";; esac
done
[ -n "$PF_DIST" ] || PF_DIST="engine/wine/dist-arm64ec-spike"
DIST_STRINGS=""
# Гейт может читаться и на PE-стороне (dlls/ntdll/loader.c → ntdll.dll), и на юниксовой
# (unix/*.c → ntdll.so). Проверять только .so значит запрещать половину рабочих наборов —
# поймано 05.08 на MACRUNNER_HB_LANGUAGE_FLOW_OBSERVER.
# ВАЖНО: на PE-стороне гейты записаны ШИРОКИМИ строками — get_env( L"MACRUNNER_..." ), то есть
# UTF-16. Обычный `strings` их не видит, и проверка ложно ругалась «нет в носителе» на гейт,
# который в модуле есть (поймано 05.08 на MACRUNNER_HB_LANGUAGE_FLOW_OBSERVER). Поэтому кроме
# обычных строк смотрим и версию с выброшенными нулевыми байтами — она ловит UTF-16.
MR_CARRIERS=()
for f in "$PF_DIST"/lib/wine/aarch64-unix/ntdll.so "$PF_DIST"/lib/wine/aarch64-unix/xtajit.so \
         "$PF_DIST"/lib/wine/aarch64-unix/xtajit64.so "$PF_DIST"/lib/wine/aarch64-unix/win32u.so \
         "$PF_DIST"/lib/wine/aarch64-windows/ntdll.dll "$PF_DIST"/lib/wine/aarch64-windows/win32u.dll \
         "$PF_DIST"/lib/wine/aarch64-windows/user32.dll \
         "$PF_DIST"/lib/wine/x86_64-windows/mono-profiler-hk_language.dll \
         "$PF_DIST"/lib/wine/aarch64-windows/mono-profiler-hk_language.dll; do
  [ -f "$f" ] && MR_CARRIERS+=("$f")
done
# Наблюдатель Mono — ОТДЕЛЬНЫЙ носитель гейтов, и это не мелочь: MACRUNNER_HB_START_GAME_ACTUATOR,
# LANGUAGE_FLOW_OBSERVER_MAX, CAMERA_PROBE и прочие читаются ИМ, а не движком. 05.08 он оказался
# не установлен в dist (лежал только в dist-arm64ec-spike), и игра честно ждала нажатия кнопки,
# которое некому было сделать: UIManager::MakeMenuLean вызывается только из RunStartNewGame и
# RunContinueGame, оба — действия игрока. Веха меню недостижима БЕЗ этого модуля.
# Извлечение имён делает python, а не конвейер из tr/strings: на PE-стороне гейты записаны
# ШИРОКИМИ строками (UTF-16), а попытка убрать нулевые байты через `tr -d` внутри двойных
# кавычек молча ломается — одинарные кавычки там литеральные, и tr выбрасывал не то.
# Из-за этого проверка ложно ругалась на MACRUNNER_HB_LANGUAGE_FLOW_OBSERVER, который в
# модуле есть. Разбор бинаря — работа для инструмента, который умеет читать байты.
DIST_STRINGS="$(python3 - "${MR_CARRIERS[@]}" <<'PYEOF'
import re,sys
names=set()
pat=re.compile(rb'MACRUNNER_[A-Z0-9_]+')
for path in sys.argv[1:]:
    try: data=open(path,'rb').read()
    except OSError: continue
    names.update(m.decode() for m in pat.findall(data))                 # обычные строки
    names.update(m.decode() for m in pat.findall(data.replace(b'\x00',b'')))  # широкие (UTF-16)
print("\n".join(sorted(names)))
PYEOF
)"
for kv in "${ENVS[@]}"; do
  name="${kv%%=*}"
  case "$name" in MACRUNNER_HB_*) ;; *) continue;; esac
  if ! printf '%s' "$DIST_STRINGS" | grep -qFs -- "$name"; then
    note "FAIL: $name нет ни в одном носителе $PF_DIST — собрано, но не задеплоено"
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

# ── 3b. Графика живёт ВНЕ диста, и её теряют при сборке набора «снизу». ───────────────────────
# 05.08: набор переменных строился от минимума, MACRUNNER_GRAPHICS_BACKEND=dxmt потерялся,
# игра ушла через wined3d+OpenGL и умерла на assert в dlls/dxgi/factory.c:559 (exit=3).
# Полдня диагностики дефекта, которого не было. Намерение обязано быть заявлено, а не угадано.
gfx=""
for kv in "${ENVS[@]}"; do [ "${kv%%=*}" = "MACRUNNER_GRAPHICS_BACKEND" ] && gfx="${kv#*=}"; done
if [ "$gfx" = "dxmt" ]; then
  dxmt_root=""
  for kv in "${ENVS[@]}"; do [ "${kv%%=*}" = "MACRUNNER_DXMT_ROOT" ] && dxmt_root="${kv#*=}"; done
  [ -n "$dxmt_root" ] || dxmt_root="engine/graphics/dist/dxmt"
  if [ -f "$dxmt_root/aarch64-windows/dxgi.dll" ]; then
    note "графика: dxmt, свой dxgi на месте ($dxmt_root)"
  else
    note "FAIL: MACRUNNER_GRAPHICS_BACKEND=dxmt, но $dxmt_root/aarch64-windows/dxgi.dll отсутствует."
    note "      Без него d3d11 уйдёт на wined3d и упрётся в assert dlls/dxgi/factory.c:559."
    fail=1
  fi
elif [ "${PREFLIGHT_NO_GRAPHICS_OK:-0}" = 1 ]; then
  note "графика не задана НАМЕРЕННО (PREFLIGHT_NO_GRAPHICS_OK=1)"
else
  note "FAIL: MACRUNNER_GRAPHICS_BACKEND не задан. Дист графику НЕ содержит — она лежит отдельно"
  note "      в engine/graphics/dist/dxmt. Прогон уйдёт на wined3d+OpenGL и умрёт на фабрике DXGI."
  note "      Если так и задумано — выставь PREFLIGHT_NO_GRAPHICS_OK=1."
  fail=1
fi

# ── 3d. Шрифты: wine грузит freetype через dlopen по ГОЛОМУ ИМЕНИ. ───────────────────────────
# 06.08: wine трижды за прогон печатал «cannot find the FreeType font library», потому что
# libfreetype есть только в /opt/homebrew/lib, которого нет в путях поиска dyld. otool -L при
# этом показывает пусто — ложное «поддержки нет». Лечится копией в lib/wine/aarch64-unix
# (scripts/mr-fonts.sh), этот каталог уже первый в DYLD_LIBRARY_PATH.
if [ -d "$PF_DIST/lib/wine/aarch64-unix" ]; then
  if scripts/mr-fonts.sh --check "$PF_DIST" >/dev/null 2>&1; then
    note "шрифты: freetype+fontconfig в дисте, dlopen по голому имени проходит"
  elif [ "${PREFLIGHT_NO_FONTS_OK:-0}" = 1 ]; then
    note "шрифты отсутствуют НАМЕРЕННО (PREFLIGHT_NO_FONTS_OK=1)"
  else
    note "FAIL: freetype/fontconfig не грузятся по голому имени из $PF_DIST."
    note "      Wine напечатает «cannot find the FreeType font library», TrueType-шрифтов не будет."
    note "      Лечится одной командой: scripts/mr-fonts.sh \"$PF_DIST\""
    fail=1
  fi
fi

# ── 3e. Файлы САМОЙ ИГРЫ: отключённый Galaxy64.dll ломает путь отказа. ───────────────────────
# 06.08: оба Galaxy64.dll были переименованы в .ОТКЛЮЧЁН. GalaxyCSharpGlue.dll его ИМПОРТИРУЕТ,
# поэтому вместо штатного «GOG failed to initialize» игра получала DllNotFoundException, на
# который её обработчик не рассчитан: DesktopPlatform.Awake() не завершался и вставало всё.
# Переименование не меняет mtime — по датам это не находится. Вернули → экран языка на экране.
HK_DIR="$(ls -d "/Users/timurtoby/Documents/MacRunner/Main/game-hollow.knight-"*/extracted-hollow-knight-* 2>/dev/null | head -1)"
if [ -n "$HK_DIR" ] && [ -d "$HK_DIR" ]; then
  off=$(find "$HK_DIR" -iname '*.dll.ОТКЛЮЧЁН' 2>/dev/null | head -5)
  if [ -n "$off" ]; then
    note "FAIL: в каталоге игры есть отключённые библиотеки — путь отказа будет НЕ тот:"
    echo "$off" | while IFS= read -r f; do note "      $(echo "$f" | sed "s|$HK_DIR/||")"; done
    note "      Вернуть: mv <файл>.ОТКЛЮЧЁН <файл>   (если так и задумано — PREFLIGHT_GAME_DLLS_OK=1)"
    [ "${PREFLIGHT_GAME_DLLS_OK:-0}" = 1 ] || fail=1
  else
    note "файлы игры: отключённых библиотек нет (Galaxy64.dll на месте)"
  fi
fi

# ── 3f. ДИСТОВ ДВА. Проверять носитель ТОГО дерева, из которого грузится ntdll.so. ───────────
# 06.08 стоило полдня: правки разворачивались в dist-arm64ec-spike, а прогон грузит ntdll.so
# из MACRUNNER_WINE_DIST (engine/wine/dist). Два замера подряд оказались недействительны —
# «прибор не печатает» и «гейт не срабатывает» читались как «явления нет».
PF_WINE_DIST=""
for kv in "${ENVS[@]}"; do
  case "${kv%%=*}" in MACRUNNER_WINE_DIST) PF_WINE_DIST="${kv#*=}";; esac
done
if [ -n "$PF_WINE_DIST" ] && [ "$PF_WINE_DIST" != "$PF_DIST" ] && [ -d "$PF_WINE_DIST/lib/wine/aarch64-unix" ]; then
  note "дистов два: запуск=$PF_DIST, ntdll.so из=$PF_WINE_DIST"
  a=$(shasum -a256 "$PF_DIST/lib/wine/aarch64-unix/ntdll.so" 2>/dev/null | cut -c1-16)
  b=$(shasum -a256 "$PF_WINE_DIST/lib/wine/aarch64-unix/ntdll.so" 2>/dev/null | cut -c1-16)
  if [ -n "$a" ] && [ -n "$b" ] && [ "$a" != "$b" ]; then
    note "FAIL: ntdll.so в двух деревьях РАЗНЫЙ ($a против $b)."
    note "      Прогон возьмёт версию из $PF_WINE_DIST — правка в другом дереве в замер НЕ попадёт."
    note "      Разложить в оба, либо выставить PREFLIGHT_DIST_SPLIT_OK=1, если так задумано."
    [ "${PREFLIGHT_DIST_SPLIT_OK:-0}" = 1 ] || fail=1
  else
    note "ntdll.so в обоих деревьях совпадает ($a)"
  fi
fi

# ── 3c. Набор переменных сверять со снимком прошлого удачного прогона, а не с памятью. ────────
if [ -n "${PREFLIGHT_REF_ENV:-}" ] && [ -f "$PREFLIGHT_REF_ENV" ]; then
  missing=$(python3 - "$PREFLIGHT_REF_ENV" "${ENVS[@]}" <<'PY'
import json,sys
ref=json.load(open(sys.argv[1]))
have={kv.split('=',1)[0] for kv in sys.argv[2:]}
want={e['name'] for e in ref['environment']['entries']
      if e['name'].startswith(('MACRUNNER_','WINEMSYNC','WINEDEBUG','DXMT_'))}
gone=sorted(w for w in want-have)
print(' '.join(gone[:12]))
PY
)
  if [ -n "$missing" ]; then
    note "FAIL: против снимка $PREFLIGHT_REF_ENV потеряны переменные:"
    note "      $missing"
    note "      Если часть выброшена намеренно — перечисли их в PREFLIGHT_DROP_OK и повтори."
    for d in ${PREFLIGHT_DROP_OK:-}; do missing="${missing//$d/}"; done
    [ -n "$(echo "$missing" | tr -d ' ')" ] && fail=1
  else
    note "набор переменных сходится со снимком $(basename "$PREFLIGHT_REF_ENV")"
  fi
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
  # Дист берём ТОТ, что реально грузится, а не жёсткую строку. 07.08 здесь стояло
  # dist-arm64ec-spike, и при запуске профиля префлайт печатал «носитель свежий
  # (e830c351be60)» — хеш рабочего дерева, тогда как прогон грузил ntdll.so профиля
  # (eb15de3eb61a). То есть проверка ручалась за файл, которого в прогоне не было:
  # ровно та ошибка, ради которой раздел 3f выше и написан. Оператор заметил по
  # несовпадению хешей в выводе.
  so="$PF_DIST/lib/wine/aarch64-unix/$m.so"
  if [ ! -f "$so" ]; then note "FAIL: носитель $m.so отсутствует в dist"; fail=1; continue; fi
  stale=$(find "$SRC" engine/hyperbridge/include \( -name '*.c' -o -name '*.h' \) 2>/dev/null \
          | while read -r f; do [ "$f" -nt "$so" ] && echo "$f" && break; done)
  if [ -n "$stale" ] && [ "${PREFLIGHT_SEALED_OK:-0}" = 1 ]; then
    # ЗАПЕЧАТАННЫЙ ЭТАЛОН СТАРШЕ ИСХОДНИКОВ ПО ОПРЕДЕЛЕНИЮ — это не поломка, а его смысл.
    # Проверка ниже ловит другое: «поправил код, собрал, разложил не туда и померил старое».
    # У прогона профиля намерение обратное — воспроизвести архивное состояние, и там возраст
    # двоичных файлов является целью. 07.08 непомеченная проверка отказала запуск эталона
    # 06.08, потому что с тех пор правился hb_pe_loader.c. Это тот же класс ошибки, что и
    # утреннее требование к профилю догонять рабочее дерево, только с другой стороны.
    # Флаг ставит mr-profile.sh, когда дист действительно лежит внутри папки профиля:
    # намерение объявляется, а не угадывается.
    note "эталон: $m.so старше исходников — так и задумано (запечатан), проверка свежести снята"
  elif [ -n "$stale" ]; then
    note "FAIL: $m.so СТАРШЕ исходников — этот носитель не пересобран (правки не в прогоне)"
    note "      первый новее: $stale"
    fail=1
  else
    note "носитель $m.so: свежий ($(shasum -a256 "$so" | cut -c1-12))"
  fi
done

# ── 5. ВСЕ дисты, участвующие в запуске, должны нести ОДИН И ТОТ ЖЕ носитель. ─────────────────
# Этот класс отказа стоил дня дважды за сутки, в двух разных местах:
#   - деплой шёл в xtajit64.so, а исполнялся ntdll.so (три носителя libhyperbridge.a);
#   - профиль эталон-меню держит ДВА диста, dist/ для запуска и wine-dist/ откуда грузится
#     ntdll.so; правки полдня разворачивались в первый, а мерился второй.
# Общее у них одно: правка ушла не в тот файл, который реально загружается. Проверка №4 выше
# смотрит только в дефолтный дист и такую пару не увидит, поэтому здесь сверяются ВСЕ дисты,
# названные окружением, и расхождение SHA — отказ, а не предупреждение.
# ВАЖНО: рабочее дерево сюда НЕ входит.
#
# Профиль (`profiles/<имя>/dist` и `wine-dist`) — ЗАПЕЧАТАННЫЙ эталон, намеренно независимый от
# рабочего дерева: `mr-profile.sh:217` подставляет его дист именно для того, чтобы прогон не
# зависел «от того, что за день сделали с engine/wine/dist».
#
# Прежняя редакция требовала совпадения профиля с рабочим деревом и тем самым заставляла эталон
# тянуться за ним. 07.08 это привело к тому, что я перезаписал двоичные файлы профиля своей
# сборкой, чтобы удовлетворить собственную проверку, — то есть проверка целостности разрушила
# то, что охраняла. Восстановлено из копий `.БЫЛО-*`.
#
# Сверять надо только дисты, участвующие В ОДНОМ запуске: у профиля это его собственные dist и
# wine-dist, и расхождение между НИМИ — настоящая ошибка (06.08 стоила полдня).
# ── Карантин macOS. Распакованный архив не запускается, и это видно только диалогом. ─────────
# 07.08: профиль распакован Keka, и она проставила com.apple.quarantine на все ~9500 файлов.
# Наши .dylib подписаны ad-hoc (Signature=adhoc, TeamIdentifier=not set), поэтому Gatekeeper
# отказывается их грузить и показывает «"libfreetype.6.dylib" Not Opened». В логе прогона при
# этом НИЧЕГО нет — отказ происходит в системе, до нашего кода. Ровно тот класс ошибки, ради
# которого писан префлайт: прибор молчит, а явление есть.
# Проверяем ДО прогона, потому что диалог ждёт мыши, а прогон тем временем идёт впустую.
check_quarantine() {
  local d="$1" n
  [ -d "$d" ] || return 0
  n=$(find "$d/lib" -name '*.dylib' -o -name '*.so' 2>/dev/null | while read -r f; do
        xattr -p com.apple.quarantine "$f" >/dev/null 2>&1 && echo x; done | wc -l | tr -d ' ')
  [ "${n:-0}" = 0 ] && return 0
  note "FAIL: в $d под карантином macOS $n двоичных файлов — Gatekeeper заблокирует загрузку."
  note "      Снять (меняются только метки, содержимое файлов не трогается):"
  # -s ОБЯЗАТЕЛЕН. Без него xattr идёт по симлинку на цель, а в префиксе Wine лежит
  # dosdevices/z: -> / — и команда упирается в «Operation not permitted» на корне тома
  # (поймано 07.08). Рекурсия внутрь / при этом не идёт: os.walk не ходит по ссылкам,
  # так что система не задета, но команда обрывается на ошибке и выглядит провалившейся.
  note "        xattr -srd com.apple.quarantine \"$d\""
  fail=1
}

DISTS=""
for v in MACRUNNER_WINE_DIST MACRUNNER_LANEA_WINE_DIST; do
  # из аргументов вызова и из унаследованного окружения — участвуют оба
  for kv in "${ENVS[@]}"; do [ "${kv%%=*}" = "$v" ] && DISTS="$DISTS ${kv#*=}"; done
  eval "inh=\${$v:-}"; [ -n "$inh" ] && DISTS="$DISTS $inh"
done
for d in $DISTS; do check_quarantine "$d"; done
for m in $CARRIERS; do
  ref=""; refd=""
  for d in $DISTS; do
    so="$d/lib/wine/aarch64-unix/$m.so"
    [ -f "$so" ] || continue
    h=$(shasum -a256 "$so" | cut -c1-16)
    if [ -z "$ref" ]; then ref="$h"; refd="$d"
    elif [ "$h" != "$ref" ]; then
      note "FAIL: $m.so РАЗЛИЧАЕТСЯ между дистами — правка попадёт не туда, где её мерят:"
      note "      $ref  $refd"
      note "      $h  $d"
      fail=1
    fi
  done
done
[ "$fail" = 0 ] && note "все дисты согласованы: $(echo $DISTS | tr ' ' '\n' | sort -u | tr '\n' ' ')"

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
