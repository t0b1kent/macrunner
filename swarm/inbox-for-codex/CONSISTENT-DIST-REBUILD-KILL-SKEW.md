# c000007b = version skew, не арка → один согласованный rebuild (выйти из whack-a-mole)

## Факт (Claude проверил file всех арок)
- x86_64-windows user32/gdi32/shell32/kernelbase/ntdll = ВСЕ `x86-64 PE32+` ✓
- aarch64-windows те же = ВСЕ `Aarch64 PE32+` ✓; explorer.exe есть в i386/x86_64/aarch64 ✓
ВЫВОД: `c000007b` (STATUS_INVALID_IMAGE_FORMAT) на user32 в explorer-path — это НЕ
wrong-arch. Гипотезу «не та арка» снять.

## Мета-проблема
Whack-a-mole: каждый раунд пересобирает ОДНУ DLL → новая loader-ошибка (missing export
→ теперь c000007b). Это симптом РАССИНХРОНА между DLL дистрибутива: часть собрана из
текущего исходника (с MACRUNNER_ARM64_MS_SYSCALL_ABI в win32u-syscalls — это и PE-thunks
в user32/gdi32, и unix win32u.so), часть — старая. Контракт import/export + syscall-ABI
разъехался → c000007b. Икон-фикс при этом цел.

## ЗАДАЧА: один СОГЛАСОВАННЫЙ rebuild+install всего затронутого набора
1. Пересобрать и установить ВМЕСТЕ, из текущего исходника, обе PE-арки + unix .so для
   всего ABI-связанного набора, чтобы контракт был единым:
   - PE (обе арки): user32, gdi32, shell32, kernelbase, ntdll, win32u (если есть PE-часть), comctl32(+v6)
   - unix .so: ntdll.so, win32u.so (с MS_SYSCALL_ABI), и зависимые
   Идеально — один `make install` дистрибутива, чтобы НЕ оставалось piecewise-skew.
2. После install: codesign где нужно (ntdll.so/win32u.so), verify-build-freshness=PASS
   (включая arch_pair_*), затем sanity: `wine user32 import chain` без c000007b.
3. Перестать пересобирать по одной DLL — именно это и плодит skew. Один консистентный
   проход > пять точечных.
4. Bounded прогон с запасом по таймауту → CGWindow появилось? Если да — один on-screen
   toolbar crop: иконки цветные?

## Решение
- Окно поднялось + иконки цветные на экране → закрыть icon-баг (ROOT-FIX в журнал),
  очистить ACTIVE-INVESTIGATION.
- Окно есть, иконки серые → новая граница present/draw активной DLL.
- c000007b остался после согласованного rebuild → это не skew; тогда winedump import
  chain user32 в explorer-процессе, найти КОНКРЕТНУЮ DLL/forwarder, что даёт invalid
  image format (PE-unixlib контракт / forwarder).

Принцип: icon ABI-фикс не трогать. verified = реальное окно. Обнови ACTIVE-INVESTIGATION.
