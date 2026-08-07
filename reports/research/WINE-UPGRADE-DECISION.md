# Обновление Wine: что мерили и что решили

Составлено 2026-08-04. Отвечает на вопрос «почему мы не на самой свежей версии Wine».

## Короткий ответ

**Мы уже на самой свежей базе, какая нам подходит.** Наше дерево — CrossOver Wine 11.0,
и CrossOver 26.3 (текущий, август 2026) стоит на том же Wine 11.0. Upstream ушёл до 11.14,
но туда нам нельзя, и вот почему.

## Ванильный Wine 11.14 — не обновление, а потеря

Наша база — не ванильный Wine, а **CrossOver Wine 11.0** от CodeWeavers
(коммит `af062458`, «Engine baseline: CrossOver Wine 11.0 + Mac patches»).

Расхождение нашего дерева с ванильным 11.0, посчитано пофайлово:

```
478 файлов   новые у CrossOver
214 файлов   изменены CrossOver
 72 файла    изменены нами
 11 файлов   новые наши
─────────────────────────────
775 файлов   всего отличий от ванильного 11.0
```

**692 файла из 775 — работа CodeWeavers, не наша.** Среди них весь слой macOS:

| файл | ванильный 11.0 | ванильный 11.14 | что это |
|---|---|---|---|
| `server/msync.c` | НЕТ | НЕТ | синхронизация без прохода через wineserver |
| `dlls/ntdll/unix/msync.c` | НЕТ | НЕТ | то же со стороны клиента |
| `dlls/winemac.drv/d3dmetal.c` | НЕТ | НЕТ | путь графики через Metal |
| `dlls/winemac.drv/d3dmetal_objc.m` | НЕТ | НЕТ | то же, Objective-C |
| `dlls/winemac.drv/cocoa_icon_utils.m` | НЕТ | НЕТ | Cocoa |

Переход на ванильный 11.14 **удалил бы msync и D3DMetal** — то, на чём стек стоит.
Мы же в msync сами чинили двойной проход через сервер (`703b8561`).

Попытка переноса это подтвердила и практически: между 11.0 и 11.14 upstream перестроил
основание — `dlls/winecrt0` → `libs/winecrt0`, `dlls/xtajit` удалён, `libs/tomcrypt`
(314 файлов) заменён на symcrypt+zlib. Пофайловая замена не собирается принципиально:
генерируемые заголовки переезжают между каталогами вместе с системой сборки.

## CrossOver 26.3 — база та же, выигрыш мал

Скачаны исходники `crossover-sources-26.3.0.tar.gz` (142 МБ). У них `VERSION` = Wine 11.0 —
**ту же базу они тоже не двигали.**

Расхождение нашего дерева с их 26.3:

```
 75 файлов   наши правки (с меткой macrunner)
  8 файлов   наши правки БЕЗ метки  ← дыра в разметке, см. ниже
 79 файлов   их починки, которых у нас нет
```

Их 79 починок — **437 строк всего**, из них **68 строк** в нашем критическом пути.
Крупнейшая — 204 строки в `dlls/msctf/threadmgr.c` (службы ввода текста, нам не нужно).
Остальное: 30 строк `advapi32/security.c`, 27 `kernel32/process.c`, 24 `win32u/winstation.c`,
дальше всё по ≤7 строк.

**Наши болевые точки они не починили.** `is_service_process()` первым в `||` при загрузке
драйвера (`dlls/win32u/driver.c`) — у них в 26.3 **посимвольно то же самое**. Дефект мёртвого
ввода ниоткуда не приедет, он наш.

## Почему не берём CrossOver целиком

Их дерево собрано под **их** систему: x86-64 у них идёт через Rosetta, у нас — через свой
транслятор. Решения, принятые в расчёте на Rosetta, для нас в лучшем случае нейтральны,
в худшем — мешают. Брать оптом означало бы тянуть чужие компромиссы и чужие ошибки
за 437 строк выигрыша.

**Порядок работы:** наше дерево — главное. CrossOver и ванильный Wine — справочники,
из которых берём поштучно и только то, что нашей задаче не противоречит.

## Дыра в разметке, которую это вскрыло

Разметка `WINE-PATCH-CLASSIFICATION.md` считает нашими файлы с меткой `macrunner`.
Нашлось **8 файлов с нашими правками без метки** — они ошибочно числились как чужие:

```
123 строки  dlls/comctl32/toolbar.c
 36         include/ntgdi.h
 25         dlls/wow64win/wow64win_private.h
 18         dlls/winemac.drv/macdrv_cocoa.h
 17         dlls/win32u/bitblt.c
 16         dlls/ntdll/unix/system.c
 11         dlls/win32u/ntgdi_private.h
 11         dlls/win32u/dib.c
```

Плюс `dlls/win32u/driver.c` — 307 строк наших правок, тоже без метки.
При правке чужого файла ставить метку, иначе следующее сравнение снова соврёт.

## РЕШЕНИЕ ОПЕРАТОРА: переезжаем на ванильный 11.14

Принято 2026-08-04. Причина — не правовая (Wine внутри CrossOver под LGPL, коммерческое
использование разрешено, предъявить по этим файлам нельзя), а инженерная: их дерево
настроено под их систему, где x86-64 идёт через Rosetta, а у нас свой транслятор.
Чужие компромиссы нам не нужны.

Единственное, что действительно нельзя брать у них — **двоичные вложения**: D3DMetal
принадлежит Apple, а не CodeWeavers. Мы им и не пользуемся, у нас DXMT.

### Цена переезда, померено

```
10 432 строки в 125 файлах  — правки CodeWeavers в нужных нам областях
                              (ntdll, winemac.drv, win32u, server, xtajit, wow64)
 3 194 строки в 19 файлах   — их настоящие новые файлы (не генерируемые)
26 914 строк наших          — лежат поверх этой базы, переносятся следом
```

Из 19 их новых файлов нам нужны немногие: msync (берём у автора, см. ниже),
`cocoa_icon_utils` (201 строка). Не нужны: `d3dmetal_objc` (у нас DXMT),
`cxmenu` (меню CrossOver), `bus_xbox360` (геймпад), `opengl_bcdec` (1337 строк, путь OpenGL).

### msync берём у автора, не у CrossOver

msync — **не работа CodeWeavers**. Это самостоятельный проект Marc-Aurel Zent
([marzent/wine-msync](https://github.com/marzent/wine-msync), LGPL-2.1). В репозитории
есть патчи `msync-cx22`/`msync-cx23` — то есть это CodeWeavers взяли msync у автора,
а не наоборот. Скачан `msync-devel.patch` (4974 строки, ~30 файлов).

Проба накладки на ванильный 11.14:

```
52 файла  наложились
 4 файла ядра msync легли ЧИСТО:
     dlls/ntdll/unix/msync.c  1689 строк
     dlls/ntdll/unix/msync.h    50
     server/msync.c            991
     server/msync.h             36
110 кусков не легли, из них:
     68 — чистая механика (get_msync_idx в object_ops) — В 11.14 НЕ НУЖНЫ,
          upstream перевёл все 36 определений object_ops на именованную
          инициализацию, пропущенные поля сами становятся NULL
     10 — генерируются из protocol.def, править не надо
     32 — НАСТОЯЩАЯ ручная работа
```

### ВАЖНО: оценка «32 куска» отменена — архитектура сервера перестроена

Оценка была сделана до того, как вскрылась перестройка. При наложении обнаружилось,
что в 11.14 upstream **вынес состояние ожидания из объектов в отдельные объекты `*_sync`**:

```c
11.0:   struct event { obj; kernel_object; manual_reset; signaled; }
        каждый объект сам несёт состояние; ops: add_queue/remove_queue/signaled/satisfied

11.14:  struct event { obj; struct object *sync; kernel_object; }
        set_event  → signal_sync( event->sync )
        reset_event→ reset_sync( event->sync )
```

`struct object_ops` при этом **сохранил** старые поля и **добавил** `get_sync` —
перестройка добавочная, а не сносящая. Состояние ожидания теперь несут всего
**три типа**: `event_sync`, `mutex_sync`, `semaphore_sync`
(плюс `inproc_sync` — обёртка Linux-драйвера, и `dxgk_shared_sync`).

Пооперационное наложение патча msync после этого бессмысленно: он написан под
архитектуру, где состояние в каждом объекте.

### Зато 11.14 даёт готовый слот для сменного движка синхронизации

Upstream сделал под свой Linux'овый `ntsync` **пластичный интерфейс**, и msync
встаёт туда же третьим движком. Это НАМНОГО меньше исходного патча.

**Сторона сервера — 9 функций, образец `server/inproc_sync.c` на 290 строк:**

```
get_inproc_device_fd()              — доступен ли движок
create_inproc_internal_sync()       create_inproc_event_sync()
create_inproc_semaphore_sync()      create_inproc_mutex_sync()
abandon_inproc_mutexes()            signal_inproc_sync()
reset_inproc_sync()                 get_inproc_sync_fd()
```

**5 точек подключения**, все одной строкой:
`server/event.c:80`, `event.c:102`, `mutex.c:146`, `semaphore.c:122`, `thread.c:518` —
везде вид `if (get_inproc_device_fd() >= 0) return create_inproc_*(...)`.

**Сторона ntdll — тот же приём:** `inproc_wait()` и соседи возвращают
`STATUS_NOT_IMPLEMENTED`, когда движок недоступен, и вызывающий падает обратно
на `server_wait`. Точки: `dlls/ntdll/unix/sync.c`, строки 766–865.

Ядро msync (991 строка на сервере, 1689 в ntdll) переписывать не надо — надо
переложить его на этот интерфейс.

### Что уже наложено в `.tmp/wine-upstream/v14-msync/`

```
4 файла ядра msync легли чисто
поле get_msync_idx в struct object_ops (server/object.h)
struct thread_data: msync_apc_addr, msync_apc_idx (unix_private.h)
их инициализация (virtual.c)
NtDelayExecution: путь msync на прерываемом ожидании (sync.c)
struct thread: msync_idx, msync_apc_idx (thread.h)
struct process: msync_idx (process.h)
protocol.def: 5 запросов msync + enum msync_type, протокол пересобран
переименование ntdll_get_thread_data → get_thread_data (8 мест)
```

### СДЕЛАНО: msync переложен на интерфейс 11.14, обе части СОБИРАЮТСЯ

```
✓ wineserver         702 344 байт, 0 ошибок
✓ ntdll.so           603 784 байт, 0 ошибок
```

**Движок дописан в `server/msync.c`** (+152 строки, файл 1143 строки) — отдельный
файл заводить не стали, чтобы не выносить наружу внутренние `get_shm`/`signal_all`:

```c
struct msync_sync { struct object obj; enum msync_type type; unsigned int shm_idx; struct list entry; };
static const struct object_ops msync_sync_ops = { .dump, .get_msync_idx, .signal, .destroy };

create_msync_internal_sync()   → MSYNC_MANUAL_SERVER / MSYNC_AUTO_SERVER
create_msync_event_sync()      → MSYNC_MANUAL_EVENT / MSYNC_AUTO_EVENT
create_msync_semaphore_sync()  → MSYNC_SEMAPHORE, ячейка low=count, high=max
create_msync_mutex_sync()      → MSYNC_MUTEX,     ячейка low=tid,   high=глубина
signal_msync_sync() / reset_msync_sync() / abandon_msync_mutexes() / get_msync_sync_idx()
```

**5 точек подключения**, каждая одной строкой перед проверкой inproc:
`event.c:81`, `event.c:104`, `semaphore.c:123`, `mutex.c:147`, `thread.c:521`.

### Грабли, на которые наступили при сборке

1. **`msync_ops` был записан позиционно**, а в 11.14 в середину `struct object_ops`
   добавлено `get_sync` — весь список поехал бы молча. Переведён на именованный.
2. **Заглушки `no_*` / `default_*` удалены в 11.14** (`no_add_queue`, `no_signal`,
   `no_get_fd`, `default_get_sd`, `no_get_full_name`, `no_lookup_name`,
   `default_unlink_name`, `no_open_file`, `no_kernel_obj_list`, `no_close_handle`) —
   при именованной инициализации они не нужны. Выжили: `default_set_sd`,
   `directory_link_name`, `no_type`, `default_map_access`, `default_get_full_name`.
3. **Обработчик `get_msync_idx` смотрел на сам объект**, а состояние с 11.14 живёт
   в отдельном `*_sync` — теперь идёт через `get_obj_sync()`, как образец.
4. **Куски старого патча в `console.c`, `event.c`, `device.c`, `queue.c`** легли, но
   ссылались на поля, которых больше нет (`server->msync_idx`, `queue->msync_idx`).
   Все они теперь лишние: эти объекты держат `sync`, созданный через
   `create_internal_sync()`, и он сам уходит в msync. Убрано 16 мест.
5. **`msync_wait_objects( ..., wait_any, ... )`** — в 11.14 параметр стал
   `WAIT_TYPE type`; заменено на `type == WaitAny`.

### Полная сборка: «Wine build complete», 0 ошибок

```
31 .so     1865 .dll     314 .exe
```

## Запуск ванили на macOS ARM64: три препятствия сняты, дошли до четвёртого

Собранное дерево **не запускалось**: `rc=137` (SIGKILL) без единой строки вывода.
Контрольное сравнение той же командой в том же каталоге:

```
ваниль 11.14 + msync:  rc=137, ноль вывода, префикса нет
наше рабочее дерево:   rc=0,   656 строк, префикс создан
```

Разбор по шагам — каждое препятствие названо точно:

**1. Предзагрузчик не собирался для aarch64.** `configure.ac`, разбор `$HOST_ARCH`:
у ванили `i386) yes`, `x86_64) …`, `*) no` — aarch64 попадает в `*`. У нас есть
строка `aarch64) wine_use_preloader=yes ;;`. Итог: `/* #undef HAVE_WINE_PRELOADER */`
против `#define HAVE_WINE_PRELOADER 1`. Без предзагрузчика загрузчик не разворачивает
адресное пространство и ядро убивает процесс на `execve`.

**2. У ванили НЕТ предзагрузчика для ARM64 вовсе.** После включения сборка встала на
`loader/preloader_mac.c:326: #error preloader not implemented for this CPU`.
Наш `preloader_mac.c` отличается **ровно на 196 строк и от ванили, и от CrossOver 26.3**
(у тех двоих файл совпадает посимвольно) — значит ARM64-предзагрузчик **написан нами**,
просто без метки `macrunner`, потому разметка и записала его в чужие.

**3. Своих флагов компоновки для ARM64 у ванили тоже нет.** Не нашлись
`mach_vm_protect`, `mach_vm_region`, `proc_regionfilename`. У нас отдельная ветка:
```
WINEPRELOADER_LDFLAGS="-nostartfiles -nodefaultlibs -Wl,-e,_start,-segalign,0x4000,\
    -pagezero_size,0x100000000,-sectcreate,__TEXT,__info_plist,... -lSystem"
WINELOADER_LDFLAGS="-Wl,-segalign,0x4000,-sectcreate,..."
```
против ванильных `-segalign,0x1000 -pagezero_size,0x1000 -ldylib1.o`.
Страницы 16 КБ и нулевая страница на 4 ГБ — это про ARM64 macOS.

**4. Текущее место остановки — разметка адресного пространства.** SIGKILL исчез,
процесс доходит до своей работы и падает осмысленно:
```
err:virtual:map_fixed_area out of memory for 0x7ffe0000-0x7ffe1000
err:virtual:virtual_alloc_first_teb wine: failed to map the shared user data: c0000017
```
Это `dlls/ntdll/unix/virtual.c` — по разметке 342 строки CodeWeavers и 2253 наших.
Следующее звено той же цепи.

### Чем это ценно

Оценка «10 432 строки в 125 файлах» перестала быть теоретической: три первых звена
названы поимённо и сняты за один заход, причём **два из трёх оказались нашей же
работой без метки**. Цепь разматывается предсказуемо — каждое препятствие называет
следующее.

## Патч Marzent оказался НЕ НУЖЕН — у нас уже есть та же схема, но живая

При первом же запуске сервер с msync из патча получил **`EXC_GUARD` на `mach_msg_trap`**:
патч зовёт этот трап напрямую, а современная macOS его сторожит и убивает процесс.

Сверка показала:

```
                          строк   mach_msg_trap
патч Marzent (server)      986         2
наше дерево  (server)      839         0     ← обновлён под современную macOS
патч Marzent (ntdll)      1689         2
наше дерево  (ntdll)      1397         0
```

И главное: **наш `server/inproc_sync.c` уже реализует msync как движок того самого
интерфейса** — 499 строк против 290 у ванили, расхождение 221 строка.
`create_msync` / `msync_set_event` / `msync_abandon_mutexes` подставлены под
`create_inproc_*` / `signal_inproc_sync` / `abandon_inproc_mutexes`.
А `inproc_sync.c` между 11.0 и 11.14 разошёлся всего на **27 строк**.

То есть всё, что я писал вручную (152 строки движка + правки в 6 файлах сервера),
у нас уже было — в рабочем, проверенном виде. Патч Marzent отброшен целиком.

## Итоговый рецепт: ванильный 11.14 + наш слой macOS

Дерево `.tmp/wine-upstream/v14-clean/` = чистый 11.14 плюс:

```
loader/preloader_mac.c              наш ARM64-предзагрузчик (196 строк, НАШ)
configure.ac + configure            3 правки: aarch64) wine_use_preloader=yes,
                                    WINELOADER_LDFLAGS и WINEPRELOADER_LDFLAGS для ARM64
include/winternl.h                  WINE_USER_SHARED_DATA_ADDRESS выше PAGEZERO
+ 7 потребителей                    virtual.c kernelbase/sync.c kernel32/sync.c
                                    kernel32/process.c ntoskrnl/instr.c win32u/message.c ntdll/thread.c
dlls/ntdll/unix/virtual.c           первый TEB не загонять под 2 ГБ
server/msync.{c,h}                  наше ядро msync (современный Mach)
server/inproc_sync.c                наше встраивание msync (переведено на именованную инициализацию)
server/main.c                       msync_init_shm() + msync_init()
server/thread.c                     alert fd: под msync отдаём индекс, не дескриптор
server/protocol.def                 поле shm_idx в ответе get_inproc_sync_fd
dlls/ntdll/unix/msync.{c,h}         клиентское ядро msync
dlls/ntdll/unix/sync.c              ветка #elif defined(__APPLE__): linux_*_obj → msync_*
dlls/ntdll/unix/loader.c            msync_init() в start_main_thread
```

Сборка: **0 ошибок**, «Wine build complete».

## Где стоим

```
✓ процесс запускается (SIGKILL ушёл)
✓ адресное пространство размечается
✓ wineserver работает
✓ wineboot.exe стартует через предзагрузчик
✓ созданы system.reg, user.reg, userdef.reg, drive_c/windows
✓ msync: bootstrapped mach port on wine-XXXXXXX-msync
✓ msync: up and running
✗ инициализация префикса не доходит до конца
```

**Стопор — рассогласование в стартовом рукопожатии.** Профили обеих сторон:

```
клиент (wineboot.exe):  signal_start_thread → init_syscall_frame → wait_suspend
                        → server_select → wait_select_reply → read   (ждёт сервер)
сервер:                 главный поток в цикле выбора, поток сообщений msync
                        в mach_msg2_trap                            (жив, ждёт)
```

Обе стороны живы и ждут друг друга.

### Разобрано по ходу отладки (и снято с подозрения)

**Приём с `/dev/null`.** Под msync наш `get_inproc_device_fd()` открывает `/dev/null`
как **пустышку**, чтобы все проверки `>= 0` прошли и рукопожатие с передачей
дескриптора состоялось; сам дескриптор не используется — ждёт msync по индексам:

```c
int get_inproc_device_fd(void)
{
    static int fd = -2;
    if (!do_msync()) return -1;
    if (fd == -2) fd = open( "/dev/null", O_CLOEXEC | O_RDONLY );
    return fd;
}
```

Файл скопирован целиком, приём на месте — не причина.

**Пути `inproc_device_fd` и `reply->inproc_device` совпадают** с нашим деревом
посимвольно (`server/thread.c`, `dlls/ntdll/unix/server.c`) — не причина.

**Клиентская сторона msync молчит вовсе:** ни `WINEDEBUG=+server`, ни `+msync,+sync`
не дают со стороны клиента ни строки, хотя серверные строки печатаются. Значит
клиент встаёт РАНЬШЕ включения своих каналов трассировки. Профиль показывает
`signal_start_thread` — то есть встаёт **вновь созданный поток**, а не главный.

### Следующий шаг

Отладка стартового рукопожатия: почему вновь созданный поток уходит в
`wait_suspend` и не получает пробуждения. Порядок проверки:
1. Убедиться, что `msync_init()` на клиенте отрабатывает ДО первого ожидания
   (сейчас он в `start_main_thread` сразу после `server_init_process()`).
2. Проверить `alert_fd` в `inproc_wait`: под msync это индекс, а не дескриптор.
3. Сверить `create_thread`/`init_thread` — кто должен снимать приостановку.

### Проверено: upstream нашу задачу НЕ решил

В 11.14 появился `server/inproc_sync.c` и поле `alert_fd`, но это путь через
`/dev/ntsync` — **драйвер ядра Linux**. На macOS не работает. `NtDelayExecution`
с прерываемым ожиданием там по-прежнему идёт в `server_wait`, то есть тот же проход
через wineserver, который мы чинили. msync обязателен.

### Порядок переезда

1. **msync** — 32 куска, описаны выше. Дерево с наложенным патчем:
   `.tmp/wine-upstream/v14-msync/` (файлы `.rej` на месте, разбор в `/tmp/hunk-class.txt`).
2. **winemac.drv** — 3688 строк CodeWeavers + 6 файлов, которых в ванили нет.
   Самая большая часть. Наши 2385 строк лежат поверх.
3. **ntdll** — loader.c(709) unix/loader.c(617) unix/signal_x86_64.c(609)
   unix/process.c(517) unix/virtual.c(342) unix/system.c(330).
4. **win32u** — window.c(397) class.c(394) dce.c(264).
5. Наши 26 914 строк — последними, когда база встанет.

Рабочее дерево остаётся главным, пока ванильное не пройдёт те же прогоны.

## Что осталось на будущее

- Ставить метки в 9 файлах, где наши правки без метки, — иначе разметка снова соврёт.
- Следующий раз, когда CrossOver сдвинет базу с 11.0 — сравнить заново той же командой.

## Как повторить измерение

```bash
# базы для сравнения лежат в .tmp/wine-upstream/:
#   wine-11.0/  wine-11.10/  wine-11.14/  cx26.3/sources/wine/
# сравнение — пофайловый cmp + признак метки macrunner, скрипты в истории сессии
```
