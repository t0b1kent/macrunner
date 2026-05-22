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

## Net-new (что закрыть ДАЛЬШЕ — из аудита Cline)
### 1. Дополнительные mapping-dependent ABI поверхности (тот же класс, что NtMapViewOfSection)
- **win32u/winstation.c** — проверить host-ARM64 ↔ ntdll unix boundary ABI.
- **win32u/dce.c** — DC/visible region путь через unix-слой.
- **win32u/dib.c** — DIB unix macro/implementation split (особенно важно — icon/bitmap путь!).
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
1. win32u/dib.c unix split — ближе всего к icon/bitmap пути (приоритет для иконок).
2. GdiBatchCount→TEB64 / PEB64 shared-addr — следующий после KUSER, тот же класс.
3. winstation.c / dce.c ABI boundary — для desktop/DC стабильности.
4. Compact ABI gating остальные callsites — системная хрупкость.

ПРИМЕЧАНИЕ: это частичный рескью (видимая часть панели Cline). Полнота не гарантирована
— Codex по ходу может найти ещё. Но класс и направления подтверждены.
