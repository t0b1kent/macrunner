# Cross-Arch ABI / Bootstrap Map (rescued from Cline session)

Date: 2026-05-22
Источник: аудит Cline по CLINE-CROSS-ARCH-ABI-BOOTSTRAP-FULL-MAP — Cline завис на
shell-quoting (`cmdand dquote>`) при записи, поэтому находки спасены вручную (Claude)
из его панели. READ-ONLY, движок не правился. Это ПОЛНАЯ карта класса, который Codex
закрывает по одной — чтобы закрыть пачкой.

## Уже сделано Codex (не дублировать)
- GDI init-order: gdi_init перед shared_session_init (журнал 18:57).
- NtMapViewOfSection ARM64 unix ABI boundary (20:15).
- KUSER_SHARED_DATA address на Apple ARM64 (20:20).
- win32u mapping-dependent callsites переведены на `win32u_map_view_of_section`:
  `winstation.c`, `dce.c`, `dib.c`. Evidence: clean Notepad++ bootstrap run
  `reports/phase-h/npp-x64-20260522-201955` без native faults, `0x7ffe0*`,
  `Cannot get server thread queue`, `failed to create desktop window`.
- Visual gate harness fixed: старый 40s wait давал full-screen fallback и ложный
  verdict. `scripts/visual-gate-notepad.sh` теперь ждёт окно через `VG_PROBE_TIMEOUT`
  (default 300), пробует `screencapture -l <window_id>`, и не считает full-screen
  fallback валидным result.

## Текущий валидный visual checkpoint (2026-05-22 21:20)
- Run: `reports/phase-h/npp-x64-20260522-212025`.
- Capture: `reports/visual-regression/notepad-gate/gate-screenshot-main.bmp`
  (`1784x1210` window BMP, не full-screen).
- Window readiness: CG window appeared on probe attempt 41 (`waited 80s`), so any
  40s visual wait is invalid for this path.
- RESULT: FAIL, но **folder_icons SKIP** (`dialog not detected`) — не использовать этот
  run как proof для folder/icon DIB bug.
- Active fails: toolbar black_ratio, bottom/status black bands, dialog_top_band region,
  scrollbar functional (System Events keystroke denied), Marlett center glyph slots.
- Bootstrap evidence: no native faults, no unsupported opcode, no desktop queue failure.
  Notepad++ window appears поздно; old 40s probe timeout was harness bug.

## Net-new (что закрыть ДАЛЬШЕ — из аудита Cline)
### 1. Дополнительные mapping-dependent ABI поверхности (тот же класс, что NtMapViewOfSection)
- **win32u/winstation.c** — DONE: через `win32u_map_view_of_section`.
- **win32u/dce.c** — DONE: через `win32u_map_view_of_section`.
- **win32u/dib.c** — DONE: через `win32u_map_view_of_section`.
- Остаточные direct callsites вне macOS/winemac path: `winewayland.drv/wayland_surface.c`,
  `wineandroid.drv/device.c`. Не desktop-bootstrap blocker, но тот же ABI-risk class
  если эти драйверы включаются на ARM64 host.
- Общий риск: unix macro vs implementation split (как winternl.h compact ABI).

### 2. Дубли/хрупкие цепочки shared-адресов (тот же класс, что KUSER)
- **GdiBatchCount → TEB64** — обращение к TEB64 для GdiBatchCount; проверить адрес/layout на ARM64.
- **PEB/PEB64 writes** — записи в PEB/PEB64; проверить корректность адресов (как был NULL GdiSharedHandleTable).

### 3. Compact ABI gating — хрупкость за пределами одного callsite
- `_NTSYSTEM_` compact ARM64 ABI vs public Win32 ABI: Codex уже патчил winternl.h на
  одном месте — но есть **prototype-context и extra-contract consistency** проблемы
  В ДРУГИХ местах (gating не покрывает все вызовы). Найти остальные callsites, где
  host-ARM win32u и ntdll-only ABI расходятся.

## Рекомендация Codex (порядок, тот же доказательный класс)
1. GdiBatchCount→TEB64 / PEB64 shared-addr — следующий после KUSER, тот же класс.
2. Compact ABI gating остальные callsites — системная хрупкость; сначала классифицировать,
   какие реально входят в macOS/winemac bootstrap path.
3. Остаточные non-winemac `NtMapViewOfSection` callsites — семейный hardening, не текущий
   Notepad++ desktop blocker.
4. После desktop-bootstrap clean-run — реальный on-screen visual verify, без Wine-render
   патчей и без возврата к ImageList guessing.

ПРИМЕЧАНИЕ: это частичный рескью (видимая часть панели Cline). Полнота не гарантирована
— Codex по ходу может найти ещё. Но класс и направления подтверждены.

## Doaudit: shared-addr/ABI остаток (Cline)

| пункт | файл:строка | риск | гипотеза фикса | приоритет |
|---|---|---|---|---|
| GdiBatchCount → TEB64: источник адреса и потребители | `dlls/ntdll/unix/virtual.c:4365`, `dlls/ntdll/unix/server.c:1714`, `dlls/win32u/winstation.c:809`, `dlls/ntdll/ntdll_misc.h:107`, `dlls/ntdll/unix/unix_private.h:72` | `GdiBatchCount` (ULONG) используется как carrier указателя `TEB64*`; при ошибке offset/packing/ширины возможен неверный cast и чтение/запись в чужую память на ARM64 host | Централизовать helper для wow-teb derivation (один источник истины), добавить жёсткие compile-time/layout assertions рядом с derivation, и runtime guard на каноничность адреса перед deref | P0 |
| PEB64 write: mirror GdiSharedHandleTable | `dlls/win32u/gdiobj.c:576-578`, `dlls/win32u/gdiobj.c:583`, `include/winternl.h:1021` | Прямые записи в `peb64->GdiSharedHandleTable` и `NtCurrentTeb()->Peb->GdiSharedHandleTable`; при неверном `teb64->Peb` повторяется класс бага “NULL/битый shared table ptr” | Перед записью в `PEB64` добавить invariant-check chain (`GdiBatchCount != 0`, `teb64->Peb != 0`, базовая валидность диапазона/выравнивания); логировать деградацию и fallback на безопасный path без mirror-write | P0 |
| PEB64 derivation path (wow_peb) | `dlls/ntdll/unix/server.c:1716`, `dlls/ntdll/unix/virtual.c:4371`, `dlls/ntdll/locale.c:92-97` | Вычисление `wow_peb` через `page_size`/offset и дальнейшее использование в ntdll locale path — потенциально хрупко при изменении bootstrap layout | Явно документировать формулу derivation рядом с кодом + добавить cross-check (teb64->Peb согласован с ожидаемым wow_peb), чтобы ранний assert ловил drift layout | P1 |
| Дополнительные TEB64 write/read callsites (TLS/ArbitraryUserPointer) | `dlls/win32u/winstation.c:882-888`, `dlls/ntdll/unix/virtual.c:3882-3895` | Несколько мест напрямую трогают `NtCurrentTeb64()->...`; при рассинхроне derivation получим silent corruption, не только падение | Свести доступы через небольшой wrapper API (get/set wow teb fields) и включить единый guard/telemetry там | P1 |
| Compact ABI gating: `_NTSYSTEM_ && __aarch64__` dual prototype for `NtMapViewOfSection` | `include/winternl.h:4631`, `include/winternl.h:4700-4703`, `dlls/ntdll/unix/virtual.c:6709-6713`, `dlls/ntdll/unix/unix_private.h:81` | Внутренний compact ABI и public ABI расходятся по сигнатуре; риск несогласованности деклараций/контекста вызова за пределами уже пропатченного места | Держать явный boundary-wrapper (как в win32u) и запретить direct calls на ABI-sensitive API в host-ARM путях через grep gate/CI rule | P0 |
| `NtMapViewOfSectionEx` контракт рядом с dual-ABI зоной | `include/winternl.h:4705`, `dlls/ntdll/unix/virtual.c:6800`, `dlls/ntdll/unix/server.c:637` | Ex-вариант живёт рядом с dual ABI зоной; при частичном hardening возможно контрактное расхождение между declaration/use path | Проверить единообразие call-contract (типов/порядка аргументов) на ARM64 unix boundary и добавить targeted ABI conformance test fixture | P1 |

Короткий вывод для Codex:
- Сначала закрывать `P0`: carrier-pointer (`GdiBatchCount -> TEB64`) и dual-ABI boundary (`NtMapViewOfSection`), это один класс bootstrap-риска.
- После этого `P1`: унификация access wrappers и контрактные проверки для `...Ex`/locale-path.
