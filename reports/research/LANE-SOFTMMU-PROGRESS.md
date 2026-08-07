04:21 · старт softmmu-исследования · прочитываю контекст (ROSETTA-DEEP, наш диспетчер mem-path) · далее: FEX/box64/QEMU источники
04:35 · контекст прочитан (ROSETTA-DEEP 323 строки, пересечений с softmmu-темой почти нет — не дублирую)
05:08 · [CODE] наш mem-path прочитан: JIT helper → hb_memory_host_ptr (hot_cache 16-slot + gap cache + treap O(log n) + guest32 нормализация) НА КАЖДЫЙ доступ; иначе hb_memory_read/write с sigsetjmp+mach_copy для guest32 · веб: FEX/box64/QEMU
