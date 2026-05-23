# E2E: запустить NPP через MacRunner.app — но на ФИКСНУТОМ движке (иначе стейл-регресс)

## Контекст
Secondary-window баг закрыт (Find/menu/Open работают, verified в ACTIVE-INVESTIGATION).
Пользователь хочет финальный продуктовый E2E: запустить Notepad++ через реальное
приложение MacRunner (Control Center), не через dev-скрипт run-notepad-x64.sh.

## РИСК (verified из кода app)
`app/.../Services/Packaging/EngineEnv.swift`:
- engineRoot() = если в бандле app есть `engine` → берёт БАНДЛ-копию (может быть стейл,
  до сегодняшних win32u/loader/ntdll/HB фиксов); иначе macRunnerRoot/engine (живое дерево).
- App использует СВОЙ bottle-prefix (не dev prefix-npp-x64-current, что мы синкали).
Значит запуск через app легко пойдёт на СТАРОМ движке/prefix → чёрные иконки / падающие
меню = НЕ новые баги, а стейл. Тест будет ложным.

## ЗАДАЧА: сделать честный E2E
1. Убедись, что app возьмёт ЖИВОЙ движок с сегодняшними фиксами:
   - либо запускать app из dev-дерева (engineRoot → macRunnerRoot/engine = с фиксами),
   - либо если app собран с бандл-engine — пересобрать/обновить бандл до текущего dist.
   Подтверди, какой engine путь реально резолвится (залогируй MACRUNNER_ENGINE_ROOT).
2. Bottle-prefix app: создай свежий / re-boot, чтобы подтянулись ФИКСНУТЫЕ DLL
   (user32/gdi32/win32u/ntdll/comctl32 + winsxs comctl32_v6). Тот же prefix-drift класс —
   verify-build-freshness-логика применима: prefix не старше dist.
3. Запусти Notepad++ через app, доведи до окна. Подтверди, что НЕ регресс:
   toolbar-иконки цветные, File-меню открывается, Open/Find-диалог создаётся.
4. Если регресс есть — сначала докажи это стейл-engine/prefix (сверь timestamps/версии),
   НЕ диагностируй как новый баг, пока не исключил стейл.

## Verify
NPP через MacRunner.app: окно, цветные иконки, меню/диалоги работают — как в dev-харнессе.
Это закрывает «работает в реальном продукте, не только в тестовом скрипте».
Готов отчёт: какой engine/prefix использовал app + результат на экране.
