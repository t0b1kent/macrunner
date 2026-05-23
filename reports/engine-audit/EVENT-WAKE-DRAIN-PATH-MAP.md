# EVENT-WAKE-DRAIN-PATH-MAP

## Краткий вывод (expected wake path)

Ожидаемая цепочка по коду в форке:

1. `winemac.drv` создаёт per-thread event queue и регистрирует её fd в wineserver через `set_queue_fd`.
2. При появлении данных на queue fd серверный poll callback ставит `QS_DRIVER` для message queue этого thread.
3. GUI thread в `NtUserMsgWaitForMultipleObjectsEx()` всегда ждёт `server_queue` handle (добавляется в список ожидания).
4. В `wait_message()` до/после `NtWaitForMultipleObjects()` вызывается `process_driver_events()`; при `QS_DRIVER` вызывается `user_driver->pProcessEvents()` (macdrv), который дренит mac event queue.
5. После drain сервер обновляет маски/биты через `set_queue_mask` и цикл продолжает pump.

Где вероятнее всего рвётся для NPP (как статическая гипотеза по коду):
разрыв между «fd signaled → `QS_DRIVER` выставлен» и фактическим участием именно NPP GUI thread в цикле `wait_message() -> process_driver_events()` (ownership/routing/liveness конкретной thread queue). Это согласуется с наблюдением «explorer дренит, NPP — нет».

---

## Таблица звеньев wake/drain

| Звено | Файл:строка | Что делает | Роль в wake/drain | Форк трогал? | Что Codex сверит |
|---|---|---|---|---|---|
| Регистрация queue fd в thread queue | `engine/wine/dlls/winemac.drv/macdrv_main.c:529-543`, `588-589` | `set_queue_display_fd()` вызывает `SERVER_START_REQ(set_queue_fd)`; `macdrv_init_thread_data()` передаёт fd созданной очереди | Связывает mac event queue конкретного GUI thread с wineserver msg queue | Лог-инструментация MacRunner (`trace_ui_input_*`) вокруг `set_queue_display_fd` (`545-551`, `590-597`) | Что fd/queue у NPP thread реально установлены и не перепривязаны |
| Серверный poll queue fd -> QS_DRIVER | `engine/wine/server/queue.c:1351-1359` | `msg_queue_poll_event()` на fd event ставит `set_queue_bits(queue, QS_DRIVER)` | Конвертирует readiness fd в driver-wake бит очереди | По коду логика как в stock | Что у «проблемной» очереди реально поднимается `QS_DRIVER` |
| Обработчик set_queue_fd | `engine/wine/server/queue.c:3079-3103` | `set_queue_fd` разрешён один раз, дублирует unix fd, создаёт `queue->fd`, ставит `POLLIN` | Владелец queue fd закрепляется за текущей thread queue | По коду логика как в stock | Что у NPP нет отказа/невалидного fd на этом шаге |
| Проверка internal bits и вход в драйвер | `engine/wine/dlls/win32u/message.c:3401-3413`, `3415-3433` | `process_driver_events()` читает `QS_DRIVER`; при сигнале вызывает `user_driver->pProcessEvents(events_mask)` | Точка фактического перехода в macdrv drain | Да: добавлены `macrunner-ui-input` trace enter/exit (`3422-3429`, `3455-3462`) | Есть ли вообще входы NPP thread сюда после фриза |
| Очистка/ресет после drain | `engine/wine/dlls/win32u/message.c:3433-3443` + `engine/wine/server/queue.c:3107-3134` | `set_queue_mask(poll_events=drained)` -> сервер `clear_queue_bits(QS_DRIVER)` и перевооружает `POLLIN` | Закрывает текущий wake-cycle и подготавливает следующий | По коду логика как в stock | Не застрял ли NPP в состоянии, где маски не обновляются корректно |
| Wait loop вокруг server queue | `engine/wine/dlls/win32u/message.c:3499-3573` | `wait_message()` делает pre-driver check, затем `NtWaitForMultipleObjects`, затем post-driver, если проснулся server_queue (`ret == count-1`) | Главный цикл wake->drain для GUI thread | Да: подробные trace-точки `wait_message_*` (`3516-3523`, `3546-3561`, `3565-3571`, `3586-3592`) | Что NPP thread продолжает входить в этот цикл после `queue_signal` |
| Добавление server queue в MsgWait | `engine/wine/dlls/win32u/message.c:3649-3654` | `NtUserMsgWaitForMultipleObjectsEx()` добавляет `get_server_queue_handle()` к handle list | Гарантирует, что GUI wait слушает server queue | По коду логика как в stock | Что NPP GUI thread ждёт именно свою server queue |
| Получение server queue handle | `engine/wine/dlls/win32u/message.c:3350-3366` + `engine/wine/server/queue.c:3058-3067` | Клиент запрашивает queue handle/idle_event у сервера | Связь user-side wait и server-side queue object | По коду логика как в stock | Нет ли рассинхронизации handle/thread |
| Реальный drain mac events | `engine/wine/dlls/winemac.drv/event.c:562-621` | `macdrv_ProcessEvents(mask)` копирует события из queue (`589`), обрабатывает (`599`), возвращает состояние fd (`611`) | Непосредственный drain из mac queue в Wine events | Да: `macrunner-ui-input` trace enter/dequeue/handled/exit (`574-619`) | Что у NPP thread есть dequeue/handled после signal |
| Поведение loop в drag path (доп. MsgWait consumer) | `engine/wine/dlls/winemac.drv/window.c:1990` (по grep hit) | В drag loop также вызывается `NtUserMsgWaitForMultipleObjectsEx` | Альтернативный consumer wait-пути в UI-сценариях | По коду совпадает со stock hit | Не «захватывает» ли wait другой поток/режим в NPP-сценарии |

---

## NPP-поток vs explorer-поток: что определяет, кто дренит driver events

По коду дренит **не процесс в целом**, а конкретный thread, у которого:

1. Инициализирован `macdrv_thread_data` и создан собственный `data->queue` (`macdrv_main.c:564-589`).
2. Этот queue fd зарегистрирован в его msg queue (`set_queue_fd`).
3. Этот же thread реально крутит `NtUserMsgWaitForMultipleObjectsEx -> wait_message` (`message.c:3627-3654`, `3499-3595`).
4. На его queue приходят `QS_DRIVER` биты (`server/queue.c:1358`).

Отсюда статическая интерпретация различия:

- **Explorer working path**: thread ownership + wait loop + QS routing остаются консистентны, поэтому `process_driver_events()` регулярно заходит в `macdrv_ProcessEvents()`.
- **NPP freeze path**: события могут продолжать signal-иться на каком-то queue fd, но целевой NPP GUI thread больше не проходит нужный цикл wait/drain (или проходит другой thread/queue), поэтому фактического drain нет.

Это именно карта expected routing-а, не утверждение о конкретном фиксе.

---

## Форк vs upstream Wine 11.0 (реальная сверка)

### Что сверялось

- Fork файлы:
  - `engine/wine/dlls/win32u/message.c`
  - `engine/wine/dlls/winemac.drv/macdrv_main.c`
  - `engine/wine/dlls/winemac.drv/window.c`
  - `engine/wine/server/queue.c`
- Upstream 11.0 (wine-mirror raw):
  - `/tmp/wine11-message.c`
  - `/tmp/wine11-macdrv_main.c`
  - `/tmp/wine11-window.c`

### Наблюдаемые расхождения по wake/drain пути

1. **`win32u/message.c`: функциональная логика wait/drain совпадает, но в форке добавлена детальная MacRunner-трассировка.**
   - Upstream ключевые точки: `process_driver_events` ~`3243`, `wait_message` ~`3305`, `NtUserMsgWaitForMultipleObjectsEx` ~`3378`.
   - Fork те же функции: `3415`, `3499`, `3627`.
   - В форке добавлены `macrunner-ui-input` trace-stages (enter/exit/pre/post wait), отсутствующие в stock.

2. **`winemac.drv/macdrv_main.c`: путь `set_queue_fd` функционально эквивалентен stock, в форке добавлены trace-маркеры.**
   - Upstream hit: `set_queue_fd` ~`489`.
   - Fork hit: `539` + дополнительный trace/logging.

3. **`winemac.drv/window.c`: MsgWait callsite в drag-пути присутствует и в stock, и в форке (по найденному upstream hit).**
   - Существенного форк-специфичного изменения wake/drain логики в этой точке не выявлено; основной форк-след — наблюдаемость через трассировку.

4. **`server/queue.c`: серверная механика `QS_DRIVER`/`set_queue_fd`/`set_queue_mask` в текущем форке выглядит canonical.**
   - `msg_queue_poll_event -> set_queue_bits(QS_DRIVER)` (`1358`),
   - `set_queue_fd` (`3079+`),
   - `set_queue_mask` с `clear_queue_bits(QS_DRIVER)` при `poll_events` (`3115-3119`).

Итог по fork-vs-stock для этой цепочки: на видимых участках это в основном **инструментированный stock-путь**, а не явный алгоритмический divergence wake/drain.

---

## Что Codex проверяет по этой карте (expected vs observed)

1. Совпадение `NPP GUI thread` ↔ `set_queue_fd` queue ownership (`macdrv_main.c` + server `set_queue_fd`).
2. Наличие `QS_DRIVER` для той же queue (`server/queue.c:1358`).
3. Непрерывность входов этого же thread в `wait_message()`/`NtWaitForMultipleObjects()` (`message.c:3499+`).
4. Наличие входов `process_driver_events()` и downstream `macdrv_ProcessEvents()` dequeue/handled (`message.c:3415+`, `event.c:589+`).
5. Где именно обрывается цепочка для NPP: до wait, в wait, между wait и post-driver, или в самом drain.

Это даёт референс «как должно идти по коду» для точного сопоставления с live-трассой без предложений фикса.