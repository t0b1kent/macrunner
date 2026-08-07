# LANE-AOTCACHE PROGRESS

Формат: `ВРЕМЯ · действие · результат · следующий шаг`

04:32 · старт лайна AOTCACHE · задача: устройство AOT-кэшей (Rosetta /var/db/oah, Prism, FEX/box64 persistence) · создание этого файла, затем веб-ресёрч Rosetta oah
04:33 · сеть: Wikipedia ок, raw.githubusercontent 404 · буду использовать html-страницы github/блоги · далее: читать ROSETTA-DEEP-ANALYSIS.md чтобы не дублировать
04:35 · FEX Config.json.in: нашёл EnableCodeCachingWIP/LazyCodeCachingWIP/CodeCacheValidation + SMCChecks=none/mtrack/full · далее: найти файлы реализации кэша + box64 dynacache
04:38 · FEX CodeCache.cpp/.h изучен: FXCC-формат, guest-rebase offsets, relocations, lazy mmap PROT_NONE → FinalizeCodePages · next: AOTGenerator + Prism + box64
04:40 · AOTGenerator.sh+cpp изучены: fileid-имя с L/T/S флаг-метками, стартовые точки = ELF symbols + unwind + endbr64 + E8-call scan · next: Prism источник
04:43 · box64 USAGE.md: DYNACACHE=1 default, FOLDER/LIMIT=2048MiB/COMPRESS=zlib fastest/MIN=30KB · next: dynacache.c реализация формата
04:42 · box64 DynaCache [CODE]: ~/.cache/box64, per-бинарь <base>-<settings>-<hash>.box64, header v?, zlib COMP_Z дефолт, min 30KB, LRU до 2048MiB, delta_map при ASLR · next: Prism источники
04:45 · Prism: только MS Learn высокого уровня (кэш per-module, сервис, совместим между приложениями); детализированных статей не нашёл · next: Prism поиск по keywords в Windows SDK + старт писать DEEP-AOTCACHE.md
04:47 · Prism: публичных деталей кэша мало, фиксирую что есть (per-module кэш, сервис, общий между приложениями); qemu TCG caching схема базово понятна (TranslationBlock hash table, jump linking, page-based invalidation) · next: пишу DEEP-AOTCACHE.md
04:48 · QEMU TCG изучен по translate-all.c: in-memory TB, encode_search sleb128 delta таблица host pc→guest data, tb_flush на page-protection; persistent AOT у QEMU НЕТ · next: пишу DEEP-AOTCACHE.md
