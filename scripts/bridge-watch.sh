#!/usr/bin/env bash
# МОСТ: внешний агент предлагает — эта машина проверяет.
#
# Зачем. Облачный агент (Qwen Coder и подобные) физически не может прогнать наш код: нужны
# macOS на Apple Silicon, Metal, Mach, ARM64EC, дист Wine на гигабайты и сама игра. Значит его
# выход — всегда гипотеза. Мост замыкает цикл: агент кладёт ветку с заявкой, эта машина собирает
# и меряет, результат уезжает обратно. Думает облако, меряет железо.
#
# ── БЕЗОПАСНОСТЬ: это исполнение чужого кода на вашей машине ─────────────────────────────────
# Не иллюзия и не теория: 2026-08-01 один из лейнов правил hb_memory.c прямо в главном дереве и
# сломал общую сборку. Здесь цена была бы выше, поэтому конструкция такая:
#   * сборка и прогон ТОЛЬКО в отдельном git worktree — главное дерево не трогается вообще;
#   * слияние в рабочую ветку НЕ ДЕЛАЕТСЯ никогда: мост только читает заявку и пишет вердикт;
#   * worktree сносится после каждой заявки, так что худшее — испорченный временный каталог.
# Решение «взять правку себе» остаётся за оператором и делается вручную.
#
# Протокол. Агент создаёт ветку `bridge/<имя>` и в ней файл `ЗАЯВКА.md` с описанием, что
# проверять. Мост находит такие ветки, прогоняет, и пишет `ВЕРДИКТ.md` в ветку `bridge-result/<имя>`.
#
# Использование: scripts/bridge-watch.sh [интервал-секунд]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
INTERVAL="${1:-300}"
WT_BASE="/tmp/macrunner-bridge"

say() { echo "[мост $(date '+%H:%M:%S')] $*"; }
say "старт, интервал ${INTERVAL}с, worktree в $WT_BASE"

while :; do
  git fetch --prune origin >/dev/null 2>&1

  for ref in $(git branch -r --list 'origin/bridge/*' 2>/dev/null | sed 's|origin/||;s/^ *//'); do
    name="${ref#bridge/}"
    done_marker=".bridge-done-$(echo "$name" | tr '/' '-')"
    [ -f "$ROOT/.git/$done_marker" ] && continue

    say "новая заявка: $ref"
    WT="$WT_BASE/$name"
    rm -rf "$WT"; mkdir -p "$(dirname "$WT")"

    if ! git worktree add --detach "$WT" "origin/$ref" >/dev/null 2>&1; then
      say "не смог создать worktree для $ref — пропускаю"
      continue
    fi

    verdict="$WT/ВЕРДИКТ.md"
    RUN_SECS=$(grep -oE 'Секунды:[[:space:]]*[0-9]+' "$WT/ЗАЯВКА.md" 2>/dev/null | grep -oE '[0-9]+' | head -1)
    {
      echo "# Вердикт моста — $(date '+%Y-%m-%d %H:%M')"
      echo
      echo "Ветка: \`$ref\`"
      echo
      if [ ! -f "$WT/ЗАЯВКА.md" ]; then
        echo "**ОТКАЗ:** в ветке нет \`ЗАЯВКА.md\`. Мост не угадывает, что проверять."
      else
        echo "## Заявка"; echo; sed 's/^/> /' "$WT/ЗАЯВКА.md"; echo
        echo "## Сборка"; echo
        if ( cd "$WT" && SDKROOT="$(xcrun --show-sdk-path)" \
             make -C engine/hyperbridge >/dev/null 2>&1 ); then
          echo "Сборка hyperbridge: **успех**"
        else
          echo "Сборка hyperbridge: **ПРОВАЛ** — дальше не пошёл, прогон не запускался."
        fi
        echo
        echo "## Прогон"; echo
        # Оператор снял ограничение на число прогонов: мост гонит каждую заявку.
        # Слот один, поэтому прогоны СТРОГО последовательны — цикл выше ждёт освобождения
        # игры перед каждой заявкой, и здесь ждём ещё раз, потому что между сборкой и
        # запуском слот мог занять оператор.
        while [ "$(ps -Ao comm | grep -cE 'Hollow Knight\.exe|AbzuGame')" -gt 0 ]; do sleep 15; done
        if [ -n "$RUN_SECS" ] && [ "$RUN_SECS" -gt 0 ] 2>/dev/null; then
            ( cd "$WT" && ./scripts/mr-profile.sh run "эталон-меню" "$RUN_SECS" ) >/dev/null 2>&1
            rd=$(ls -dt "$WT"/reports/phase4-hollow-knight/*профиль-эталон* 2>/dev/null | head -1)
            if [ -n "$rd" ] && [ -f "$rd/run.log" ]; then
                echo "Рундир: \`$(basename "$rd")\`"; echo
                echo "Веха языка: $(grep -c 'Restored language' "$rd/run.log" 2>/dev/null) раз"
                echo
                echo "### Маркеры, заявленные в ЗАЯВКЕ.md"; echo
                # Проверяется ровно то, что агент назвал критерием — и ничего сверх.
                grep -oE 'Маркер:[[:space:]]*[^[:space:]]+' "$WT/ЗАЯВКА.md" 2>/dev/null |
                    sed 's/Маркер:[[:space:]]*//' | while read -r mk; do
                        [ -z "$mk" ] && continue
                        n=$(grep -c -- "$mk" "$rd/run.log" 2>/dev/null || echo 0)
                        echo "- \`$mk\`: $n"
                    done
                echo
                echo "Маркер с нулём — критерий НЕ выполнен. Это результат, а не повод"
                echo "переформулировать критерий."
            else
                echo "**Прогон не дал рундира** — смотреть сборку выше."
            fi
        else
            echo "В ЗАЯВКЕ.md не указано \`Секунды: N\` — прогон пропущен."
        fi
        echo
        echo "## Что мост по-прежнему НЕ делает"
        echo
        echo "Не сливает эту ветку никуда и не разворачивает её сборку в рабочие дисты."
        echo "Прогон идёт из ИЗОЛИРОВАННОГО worktree, рабочее дерево оператора не затрагивается."
      fi
    } > "$verdict"

    if git -C "$WT" checkout -q -B "bridge-result/$name" 2>/dev/null &&
       git -C "$WT" add ВЕРДИКТ.md 2>/dev/null &&
       git -C "$WT" -c user.name=macrunner-bridge -c user.email=bridge@local \
           commit -q -m "bridge: вердикт по заявке $name" 2>/dev/null &&
       git -C "$WT" push -q origin "bridge-result/$name" 2>&1 | tail -1; then
      say "вердикт отправлен: bridge-result/$name"
    else
      say "вердикт НЕ отправлен для $name (см. $verdict)"
    fi

    touch "$ROOT/.git/$done_marker"
    git worktree remove --force "$WT" >/dev/null 2>&1
  done

  sleep "$INTERVAL"
done
