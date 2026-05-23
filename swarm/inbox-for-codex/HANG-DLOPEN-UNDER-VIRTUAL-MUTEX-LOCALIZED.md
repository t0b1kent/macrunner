# Pre-wine-process-start HANG локализован (Claude): dlopen под virtual_mutex

## Точка (сильный лид, подтверди стеком — он уже у тебя)
Стек зависшего процесса (`sample-hung-wine-before-process-start-20260523-132233.txt`):
`__wine_syscall_dispatcher → NtQueryVirtualMemory → dyld dlopen → dlopen_from →
withLoadersWriteLockAndProtectedStack → notifyLoad/notifyDebuggerLoad → (рекурсивный
dlopen) → notify_register_check`. Главный поток в CFRunLoopRun (норм), worker висит тут.

Источник dlopen: **`engine/wine/dlls/ntdll/unix/virtual.c:868`** в `get_builtin_unix_funcs`:
```
server_enter_uninterrupted_section(&virtual_mutex, &sigset);   // :862
... builtin->unix_handle = dlopen(builtin->unix_path, RTLD_NOW); // :868  <-- dlopen ПОД virtual_mutex
server_leave_uninterrupted_section(&virtual_mutex, &sigset);   // :879
```
То есть `dlopen` зовётся, **держа `virtual_mutex`**. `NtQueryVirtualMemory` (:6580) тоже
берёт этот mutex. dyld во время dlopen берёт свой loader write-lock и шлёт load-
notifications, которые рекурсивно заходят в dlopen / notify — под удержанным
virtual_mutex это даёт hang/reentrancy на macOS.

## Почему именно сейчас (не в стоке)
get_builtin_unix_funcs во многом сток, НО x64-loader path (твои недавние native-
counterpart фиксы) достигает его в bootstrap-контексте/треде, где dyld notify-reentrancy
кусается, и/или dlopen грузит native-counterpart .so, дающий notify-шторм.

## Гипотеза фикса (твой слой, твоё решение)
Не держать `virtual_mutex` поперёк `dlopen` — классика «снять внутренний lock перед
dlopen»:
- освободить virtual_mutex вокруг самого dlopen (найти builtin под lock → отпустить lock
  → dlopen вне lock → снова взять lock → записать unix_handle, проверив гонку), ИЛИ
- сериализовать загрузку builtin unix .so отдельным dedicated lock'ом, не virtual_mutex, ИЛИ
- понять, ЧТО грузит этот dlopen в bootstrap и почему notify рекурсирует, и убрать
  причину notify-шторма.
Подтверди стеком/трассой какой именно unix_path грузится в момент зависания
(добавь временный fprintf перед :868: builtin->unix_path), затем фикс в правильном слое.

## Verify
После фикса bounded прогон → доходит ли до `wine-process-start` → CGWindow → toolbar crop.
Icon ABI-фикс цел. Это pre-visual host-integration boundary (dyld↔Wine VM-lock), не render.
