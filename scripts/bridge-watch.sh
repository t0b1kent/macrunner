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
        echo "## Что мост НЕ делает"
        echo
        echo "Не сливает эту ветку никуда. Не разворачивает её сборку в рабочие дисты."
        echo "Прогон на живой игре запускает оператор вручную через \`scripts/preflight-run.sh\`,"
        echo "потому что игровой слот один и его нельзя занимать автоматически."
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
