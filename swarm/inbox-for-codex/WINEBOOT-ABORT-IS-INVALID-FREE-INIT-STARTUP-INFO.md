# wineboot rc=134 (SIGABRT) ЛОКАЛИЗОВАН: invalid free в init_startup_info

## Прогресс (твой helper-redirect сработал)
get_pe_file_info теперь выбирает native helper: machine=0xaa64, x64_guest=0, нет 0x8664.
wineboot больше НЕ x64, НЕ виснет. Новый симптом: быстрый abort rc=134.

## Причина (из crash report, Claude прочитал .ips)
`~/Library/Logs/DiagnosticReports/wine-preloader-2026-05-23-143615.ips`:
SIGABRT, стек:
`___BUG_IN_CLIENT_OF_LIBMALLOC_POINTER_BEING_FREED_WAS_NOT_ALLOCATED → malloc_report →
abort  ←  NtCreateUserProcess → init_startup_info → start_main_thread`.
= heap corruption / невалидный free в `init_startup_info` (env.c:2039).

## Главный подозреваемый (file:line)
`dlls/ntdll/unix/env.c:2065`:
```
info_size = reply->info_size;
env_size = (wine_server_reply_size( reply ) - info_size) / sizeof(WCHAR);   // <-- underflow?
env = malloc( env_size * sizeof(WCHAR) );                                   // :2069
memcpy( env, (char *)info + info_size, env_size * sizeof(WCHAR) );          // :2070  heap overrun
...
free( env );   // :2140
free( info );  // :2141   <-- abort здесь, если куча уже испорчена
```
Если после смены арки wineboot серверный `get_startup_info` reply отдаёт `info_size`,
который >= reply_size (или layout под native helper иной), то `env_size` (SIZE_T)
уходит в **unsigned underflow** → гигантский memcpy → heap corruption → abort на free.
Второй подозреваемый: `add_dynamic_environment(&env,...)` (env.c:1030, зовётся :2073) —
если reassign env на non-malloc указатель.

## Что проверить (одна трасса, не итерации)
Сразу после SERVER_END_REQ (env.c:~2066) залогируй reply_size/info_size/env_size/machine.
Сравни native-helper vs прежний x64: где info_size/env_size разъехались. Это назовёт,
сломалась ли size-математика под новый arch helper. Фикс: корректный size-контракт
get_startup_info для native helper (или guard env_size от underflow) — в правильном слое
(server reply / init_startup_info), не костыль.

## Verify
wineboot --init завершается без SIGABRT и без timeout → wine-process-start → CGWindow →
toolbar crop. Icon ABI-фикс цел. Это последний bootstrap-слой перед окном.
