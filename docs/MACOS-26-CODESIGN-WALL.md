# macOS 26 Codesign Wall

## Что произошло

Мы собрали полный arm64-native Wine 11 (1011 aarch64 PE + 1005 x86_64 PE + 32 Unix host модулей, 2.5 GB). Wine `--version` отвечает корректно. Но запуск `wine notepad.exe` или `wine wineboot --init` падает с `exit 137` (SIGKILL) и пустым stderr.

## Почему

`log show` показал:

```
kernel: (AppleSystemPolicy) ASP: Security policy would not allow
process: <pid>, .../lib/wine/aarch64-unix/wine

amfid: ... not valid: Error Domain=AppleMobileFileIntegrityError
Code=-423 "The file is adhoc signed or signed by an unknown
certificate chain"
```

**macOS 26 Tahoe не разрешает `posix_spawn` adhoc-подписанных бинарей.**

Wine использует двухуровневый запуск:
1. `bin/wine` (top-level) → грузит `lib/wine/aarch64-unix/ntdll.so` (как dylib — это работает)
2. ntdll.so spawn-ит дочерний `lib/wine/aarch64-unix/wine` (loader для PE) — **здесь ASP блокирует**

Мы попробовали adhoc-подпись со всеми entitlements:
- `com.apple.security.cs.allow-jit`
- `com.apple.security.cs.allow-unsigned-executable-memory`
- `com.apple.security.cs.allow-dyld-environment-variables`
- `com.apple.security.cs.disable-library-validation`
- `com.apple.security.cs.disable-executable-page-protection`
- `com.apple.security.get-task-allow`

Не помогает. ASP всё равно отвергает на этапе spawn (loaded as library — ОК, spawned as process — нет).

## Как это решают другие

| Продукт | TeamIdentifier | Как подписан |
|---------|---------------|--------------|
| Whisky | 92S3SG4PTH | Apple Developer ID + notarization |
| CrossOver | CodeWeavers | Apple Developer ID + notarization |
| Mythic | (similar) | Apple Developer ID |

Все шипят через **Apple Developer ID** ($99/год Apple Developer Program) и **notarization**. Это не вопрос Wine — это вопрос дистрибуции на macOS 26.

## Варианты для нашего проекта

### Вариант A — Free Apple Developer + локальный self-cert
- **Стоимость:** $0
- **Что нужно:**
  1. Apple ID → Xcode → Preferences → Accounts → "Add Apple ID"
  2. Xcode сделает Free Provisioning Profile
  3. `security find-identity -v -p codesigning` после этого покажет `Apple Development: Your Name`
  4. Подписать через `codesign --sign "Apple Development: ..." --entitlements wine.entitlements ...`
- **Что даст:** работает на ЭТОЙ машине. Не работает на других без trust.
- **Подходит для:** development.

### Вариант B — Apple Developer Program
- **Стоимость:** $99/год
- **Что даст:** Developer ID Application cert. Notarization. Дистрибуция через .dmg на любой Mac.
- **Подходит для:** production / релиз.

### Вариант C — Отключить SIP и AMFI
- **Стоимость:** $0
- **Что нужно:**
  1. Перезагрузка → зажать Power → войти в Recovery Mode
  2. Terminal: `csrutil disable`
  3. Reboot
  4. `sudo nvram boot-args="amfi_get_out_of_my_way=0x1"`
  5. Reboot
- **Что даст:** запуск любых adhoc подписанных бинарей.
- **Минусы:** ослабляет всю систему. Не подходит для production.
- **Подходит для:** только если разработка важнее безопасности.

### Вариант D — Бандлить .app
- macOS иногда мягче относится к binary внутри `.app` bundle с Info.plist.
- Может частично помочь, но обычно не достаточно для top-level loader.

## Рекомендация

**Вариант A** — бесплатный Apple Developer ID + self-signed cert через Xcode.

Это что используют open-source разработчики (Whisky, Mythic в дев-билдах). После публичного релиза → переход на $99 Developer Program для notarization.

## Что делать пользователю прямо сейчас

1. Открыть **Xcode**
2. **Xcode → Settings → Accounts → "+" → Apple ID** → войти в свой Apple ID
3. Дождаться когда появится "Personal Team" в списке
4. Запустить:
   ```bash
   security find-identity -v -p codesigning
   ```
   Должно показать `Apple Development: ...`
5. Запустить `./scripts/sign-engine.sh` (создадим)

## Что мы не теряем

Весь движок собран и работает на уровне библиотек. Подпись — **последний шаг**, не пересборка. Когда сертификат появится, `codesign --force ...` за минуту перепишет подписи.

Мы НЕ начинаем сначала.
