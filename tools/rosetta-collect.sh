#!/usr/bin/env bash
# ПОЛНАЯ карта Rosetta 2 — транслятора x86-64 в macOS на Apple Silicon.
#
# Зачем. Rosetta решает ровно нашу задачу и решает её лучше: 71 % нативной скорости против наших
# ~10 %. Она стоит на этой же машине, и всё, что можно снять чтением, — бесплатная истина,
# которую не надо добывать замерами. Тот же подход, что дал по Prism: контракт из 25 функций,
# отдельные AOT-компиляторы и ответ на стену с MEM_FREE.
#
# Снимается ВСЁ, что доступно чтением, а не под текущий вопрос: собирать узко — значит через
# неделю обнаружить нехватку соседнего куска.
#
# Ничего не меняет. Только читает. Часть разделов полнее под sudo, без него тоже отработает.
#
# Запуск: ./tools/rosetta-collect.sh > reports/rosetta/ОТЧЁТ.txt 2>&1
set -uo pipefail
H() { echo; echo "==== $1 ${*:2}"; printf '=%.0s' $(seq 1 $((70 - ${#1}))); echo; }
S() { echo; echo "-- $1 --"; }

H "0. СИСТЕМА"
sw_vers 2>/dev/null
echo "процессор: $(sysctl -n machdep.cpu.brand_string 2>/dev/null)"
echo "Rosetta установлена: $([ -d /Library/Apple/usr/libexec/oah ] && echo да || echo НЕТ)"

H "1. ДВОИЧНЫЕ ФАЙЛЫ — обходом каталога, а не по списку известных имён"
# Список известных имён пропустит то, чего мы не знаем. По Prism так нашлись xtac*.exe —
# отдельные AOT-компиляторы, о которых мы не подозревали.
for d in /Library/Apple/usr/libexec/oah /usr/libexec/rosetta /Library/Apple/usr/lib; do
    [ -d "$d" ] || continue
    S "каталог $d"
    find "$d" -type f 2>/dev/null | while read -r f; do
        printf "  %12s  %-52s %s\n" "$(stat -f %z "$f" 2>/dev/null)" "${f#$d/}" \
               "$(file -b "$f" 2>/dev/null | cut -c1-40)"
    done
done

H "2. КЕШ ТРАНСЛЯЦИЙ /var/db/oah — раскладка, именование, формат"
# У нас общий кеш сдвигал вход в GOG с 537-й секунды на 184-ю. Первоклассный рычаг, поэтому
# интересно всё: где, чем именовано, какого размера, и сигнатура формата.
if [ -d /var/db/oah ]; then
    echo "  размер: $(du -sh /var/db/oah 2>/dev/null | cut -f1)"
    echo "  файлов: $(find /var/db/oah -type f 2>/dev/null | wc -l | tr -d ' ')"
    S "по расширениям"
    find /var/db/oah -type f 2>/dev/null | sed 's/.*\.//' | sort | uniq -c | sort -rn | head -8
    S "крупнейшие 10"
    find /var/db/oah -type f 2>/dev/null -exec ls -l {} + 2>/dev/null | sort -k5 -rn | head -10 |
        awk '{printf "  %12s  %s\n", $5, $NF}'
    S "сигнатура крупнейшего файла (первые 64 байта)"
    BIG=$(find /var/db/oah -type f 2>/dev/null -exec ls -S {} + 2>/dev/null | head -1)
    [ -n "$BIG" ] && xxd -l 64 "$BIG" 2>/dev/null
else
    echo "  /var/db/oah недоступен (нужен sudo) либо Rosetta не запускалась"
fi

H "3. СЛУЖБЫ И ПРОЦЕССЫ"
S "демон oahd и родня"
ps -Ao pid,comm,args 2>/dev/null | grep -iE "oahd|rosetta" | grep -v grep | head -6
S "launchd-задания"
ls /System/Library/LaunchDaemons/ 2>/dev/null | grep -iE "oah|rosetta"
S "точки монтирования (кеш может быть отдельным томом)"
mount | grep -iE "oah|rosetta"

H "4. ★ АППАРАТНЫЕ ОПОРЫ — то, чего у нас нет и что объясняет разрыв"
# Здесь главное отличие Rosetta от нас: она пользуется тем, чего сторонним не дают.
S "расширения ARM64, видимые нам"
for f in hw.optional.arm.FEAT_FlagM hw.optional.arm.FEAT_FlagM2 hw.optional.arm.FEAT_AFP \
         hw.optional.arm.FEAT_LSE hw.optional.arm.FEAT_LRCPC hw.optional.arm.FEAT_LRCPC2; do
    printf "  %-34s %s\n" "${f##*.}" "$(sysctl -n $f 2>/dev/null || echo '—')"
done
echo
echo "  ЗАМЕТКА: аппаратный TSO (ACTLR_EL1 бит 0) Rosetta включает через приватный"
echo "  entitlement com.apple.private.oahd. Сторонним недоступен; kext требует Reduced Security."
echo "  Это НЕ гипотеза — проверено ранее, см. reports/research по TSO."

H "5. ПРЕДТРАНСЛИРОВАННЫЙ КЕШ dyld ДЛЯ x86_64"
ls -la /System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/ 2>/dev/null | head -6
ls -la /System/Library/dyld/ 2>/dev/null | grep -i x86 | head -4

H "6. ENTITLEMENTS транслятора — что ему позволено сверх обычного"
for f in /Library/Apple/usr/libexec/oah/oahd /Library/Apple/usr/libexec/oah/runtime; do
    [ -f "$f" ] || continue
    S "$f"
    codesign -d --entitlements - "$f" 2>/dev/null | head -30
done

H "7. ЧЕГО ЗДЕСЬ НЕТ И ЧЕМ ЭТО ДОБИРАЕТСЯ"
cat <<'EOF'
  - Формат файлов кеша /var/db/oah: нужен разбор двоичного содержимого на стороне,
    инструмент — tools/prism-parse.py умеет PE, для Mach-O нужен свой или otool.
  - Порядок обращений при первом против повторного запуска: нужен fs_usage или dtrace
    по процессу под Rosetta, покажет очерёдность, чего никакой список файлов не даст.
  - Поведение с самомодифицирующимся кодом: у нас Mono JIT-ит на ходу. Проверяется
    запуском x86-64 приложения с JIT под Rosetta и наблюдением за /var/db/oah.
  - Внутреннее устройство трансляции: закрыто, разбирается только реверсом runtime.
EOF
echo
echo "==== КОНЕЦ ===="
