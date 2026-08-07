#!/usr/bin/env bash
# Кеш трансляций Rosetta 2 — часть, требующая root.
#
# Отдельным файлом, потому что пароль вводит только оператор. Всё остальное по Rosetta снимается
# без прав: tools/rosetta-collect.sh.
#
# Кеш — первоклассный рычаг: наш общий кеш трансляций сдвигал вход в GOG с 537-й секунды на 184-ю.
# Поэтому интересны раскладка, именование, размеры и сигнатура формата, а не только факт наличия.
#
# Только чтение. Ничего не меняет, ничего не удаляет.
#
# Запуск:  sudo ./tools/rosetta-cache-sudo.sh > reports/rosetta/КЕШ.txt 2>&1
set -uo pipefail
D=/var/db/oah
echo "==== КЕШ ТРАНСЛЯЦИЙ ROSETTA 2 ===="
[ -d "$D" ] || { echo "$D не существует — Rosetta ни разу не запускалась"; exit 0; }

echo "размер: $(du -sh "$D" 2>/dev/null | cut -f1)"
echo "файлов: $(find "$D" -type f 2>/dev/null | wc -l | tr -d ' ')"

echo; echo "-- раскладка каталогов (2 уровня) --"
find "$D" -maxdepth 2 -type d 2>/dev/null | head -20

echo; echo "-- по расширениям --"
find "$D" -type f 2>/dev/null | sed 's/.*\///; s/^[^.]*$/(без расширения)/; s/.*\.//' |
    sort | uniq -c | sort -rn | head -10

echo; echo "-- крупнейшие 12 --"
find "$D" -type f -exec ls -l {} + 2>/dev/null | sort -k5 -rn | head -12 |
    awk '{printf "  %12s  %s\n", $5, $NF}'

echo; echo "-- сигнатура формата: первые 96 байт крупнейшего файла --"
BIG=$(find "$D" -type f -exec ls -S {} + 2>/dev/null | head -1)
if [ -n "$BIG" ]; then
    echo "  файл: $BIG"
    echo "  тип:  $(file -b "$BIG" 2>/dev/null)"
    xxd -l 96 "$BIG" 2>/dev/null | sed 's/^/  /'
fi

echo; echo "-- соответствие кеша исходным двоичным файлам --"
# Ключевой вопрос: чем именованы записи — хешем файла, путём, inode? От этого зависит,
# как Rosetta находит готовую трансляцию, и можем ли мы устроить так же.
find "$D" -type f 2>/dev/null | head -5 | while read -r f; do
    echo "  $(basename "$(dirname "$f")")/$(basename "$f")"
done

echo; echo "-- владелец и права (кто может писать в кеш) --"
ls -ld "$D" 2>/dev/null
find "$D" -maxdepth 1 -mindepth 1 2>/dev/null | head -3 | xargs ls -ld 2>/dev/null
echo; echo "==== КОНЕЦ ===="
