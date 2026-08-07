#!/usr/bin/env bash
# Ловит штормящую страницу в ЖИВОМ процессе и снимает её настоящие права.
#
# Зачем. Печать fault-page в floor26 не выводит esr/dfsc, а именно dfsc отличает
# «страница не отображена» (0x07) от «права не те» (0x0f) — лечится это по-разному.
# Пересобрать floor26 нельзя: её исходников целиком нет (этажи несут два файла из ста),
# а сборка из 7935c687 даст похожую, но не ту же сборку. Зато ядро знает правду про
# отображение прямо сейчас, и vmmap её показывает — SIP снят 07.08, доступ есть.
#
# Разбор вывода делает python, а не awk: в macOS awk нет ни strtonum, ни match с тремя
# аргументами, и зонд молча не находил бы ничего — ровно тот класс молчащего прибора,
# из-за которого 07.08 потерян день.
#
# Использование: scripts/storm-probe.sh [интервал-секунд]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INT="${1:-20}"
OUT="$ROOT/reports/phase4-hollow-knight/storm-probe-$(date +%Y%m%d-%H%M%S).txt"
echo "зонд шторма -> $OUT"
seen=""
while :; do
  # ИМЯ ПРОЦЕССА, А НЕ КОМАНДНАЯ СТРОКА. pgrep -f 'game-hollow' совпадал с командной строкой
  # самого Claude, где путь к игре стоит в аргументе --add-dir: зонд семь раз подряд снимал
  # карту НЕ ТОГО процесса и уверенно докладывал «страница не отображена». Вывод получился бы
  # ПРОТИВОПОЛОЖНЫМ правильному — на деле страница отображена как r-x.
  PID=$(ps -Ao pid,comm | grep -i 'Hollow Knight.exe' | grep -v grep | awk '{print $1}' | head -1)
  [ -n "$PID" ] || { sleep "$INT"; continue; }
  D=$(ls -dt "$ROOT"/reports/phase4-hollow-knight/laneA-профиль-* 2>/dev/null | head -1)
  [ -n "$D" ] && [ -f "$D/run.log" ] || { sleep "$INT"; continue; }
  A=$(grep -o 'fault-page: addr=[^ ]* faults=[0-9]*' "$D/run.log" 2>/dev/null |
      sed 's/fault-page: addr=//; s/ faults=/ /' | sort -k2 -n | tail -1 | cut -d' ' -f1)
  [ -n "$A" ] || { sleep "$INT"; continue; }
  vmmap "$PID" 2>/dev/null > /tmp/.storm-vmmap.$$ || { sleep "$INT"; continue; }
  python3 - "$A" "$PID" /tmp/.storm-vmmap.$$ >> "$OUT" 2>&1 <<'PY'
import sys,re,datetime
addr=int(sys.argv[1],16); pid=sys.argv[2]
print(f"── {datetime.datetime.now():%H:%M:%S}  pid={pid}  страница=0x{addr:x}")
hit=False
for line in open(sys.argv[3],errors='replace'):
    m=re.search(r'([0-9a-f]{6,16})-([0-9a-f]{6,16})', line)
    if not m: continue
    lo=int(m.group(1),16); hi=int(m.group(2),16)
    if lo <= addr < hi:
        print("   ", line.rstrip()[:150]); hit=True
if not hit:
    print("    в карте процесса такой области НЕТ — страница не отображена")
PY
  rm -f /tmp/.storm-vmmap.$$
  echo "  снято: $A"
  sleep "$INT"
done
