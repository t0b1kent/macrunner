# Перспективные игры для слоя перевода Windows‑игр на ARM64 (2026)

> Источник: ChatGPT (web search), 2026-05-29. Промпт:
> `docs/CHATGPT-SEARCH-game-target-ladder-anticheat-prompt.md`.
> Сохранено как research-артефакт. Цитаты/источники — в исходном тексте ChatGPT.

## 1. Фильтр «kill filter»: античит и драйверы, несовместимые с переводом на ARM64

| Технология/игровой сервис | Доказанный уровень защиты | Причина несовместимости |
|---|---|---|
| **Easy Anti‑Cheat (EAC)** | Пользовательский драйвер + сервис; две версии: старая (Non‑EOS) и новая EOS | Для Proton нужен переход на EAC‑EOS + интеграция Epic Online Services разработчиком (нет простого переключателя). Valve/Epic упростили: студия включает Linux в панели EAC + кладёт `easyanticheat_x64.so`, но переход делает только разработчик. Без офиц. поддержки — не работает в Wine на macOS/ARM64. |
| **BattlEye** | Системный драйвер ядра (`PnkBstrK.sys`) | Kernel-driver сканирует систему. Разработчик должен сам обратиться в BattlEye для Proton-поддержки. Kernel-драйверы не идут в Wine/Proton; *PUBG*, *Rainbow Six* — несовместимы. |
| **Riot Vanguard** | Полный контроль ядра + проверка загрузки | Wine не может удовлетворить требование Vanguard по аттестации состояния загрузки/модулей ядра. Riot считает эмуляцию опасной, Linux поддерживать не планирует. |
| **Ricochet (Activision/CoD)** | Многоуровневый античит с kernel-драйвером | Драйвер ядра мониторит взаимодействие с игрой, планируется связка с TPM 2.0 + Secure Boot. Kernel-часть не работает под Wine. |
| **nProtect GameGuard** | Driver-based античит | Kernel-драйвер, несовместим с Wine/Proton (PCGamingWiki). Поддержку новой версии решает только разработчик MMO. |
| **Denuvo Anti‑Cheat** | Kernel-драйвер + служба обновления | Ставит драйвер для аппаратной защиты, несовместим с Proton (признано вендором). *FIFA 23* и др. — не запускаются. |
| **Denuvo Anti‑Tamper** (DRM, НЕ античит) | Аппаратная привязка + периодич. онлайн-подтверждение | НЕ содержит kernel-драйвер → теоретически работает через Wine, но сетевые/API проблемы могут мешать. **Серая зона, не авто-killer.** |
| **PunkBuster** | Driver `PnkBstrK.sys` | Kernel-драйвер; *Battlefield Bad Company 2* и др. несовместимы. |
| **DX12-только игры** | Отчасти устранимо | GPTK/Wine поддерживают DX11 и DX12, но DX12-only ведут себя нестабильно и требуют своей реализации DX12. *Elden Ring*, *Forza Horizon 5*, *A Plague Tale: Requiem* — **только** DX12 → сложно для нас (мы D3D11→Metal сначала). |
| **Магазинные DRM-клиенты** | Реальные препятствия | Epic Games Launcher под Wine/CrossOver регулярно ломается (краш на обязат. обновлении, не пускает в аккаунт). GOG/Steam-игры часто запускаются напрямую; *Dead Cells* DRM-free с `-EpicPortal`. |

**Общий вывод по kill-filter:** все kernel-античиты (EAC Non‑EOS, BattlEye, Vanguard,
Ricochet, GameGuard, PunkBuster) — отсев, т.к. Wine/Proton реализуют только user-space API.

## 2. Зелёный список игр-кандидатов (без kernel-античита, DX9/11/Vulkan/OpenGL/Metal)

| Игра (год) | Движок (API) | Античит/DRM | Tier |
|---|---|---|---|
| **★ Hades (2020)** | C# движок (Vulkan/OpenGL) | Нет античита; Proton стабилен, низкие требования | 1 |
| **★ Stardew Valley (2016)** | MonoGame/FNA (D3D9/11/Metal/OpenGL) | Нет античита; ProtonDB Platinum | 1 |
| **Hollow Knight (2017)** | Unity (D3D11/OpenGL/Metal/Vulkan) | Нет античита; есть нативные Linux/macOS | 1 |
| **Limbo (2011)** | Свой движок (D3D9.0c/OpenGL2.0) | Нет античита; без лаунчера | 1 |
| **Dead Cells (2018)** | Heaps (D3D9.0c/11/OpenGL3.2) | DRM-free (`-EpicPortal`); нет античита | 1 |
| **Celeste (2018)** | XNA/FNA (D3D9 / D3D11/Metal/OpenGL) | Нет античита; нативные Linux/macOS | 1 |
| **Hyper Light Drifter (2016)** | GameMaker (D3D9 / OpenGL) | Нет античита; 32-бит | 1 |
| **Cuphead (2017)** | Unity (D3D9/11, основная DX11) | Без kernel-античита (Denuvo Anti-Tamper) | 2 |
| **The Talos Principle (2014)** | Serious Engine 4 (D3D11/12, Vulkan, OpenGL) | Без античита; DX12 не обязателен | 2 |
| **Inside (2016)** | Unity 5 (D3D9/11) | DRM-free после установки; нет античита | 2 |
| **Return of the Obra Dinn (2018)** | Unity 2017 (D3D11) | Нет античита; DRM-free из папки | 2 |
| **Axiom Verge (2015)** | FNA (D3D11/OpenGL) | DRM-free; нет античита | 2 |
| **The Witness (2016)** | Свой движок (D3D11/Metal) | Нет античита; есть macOS-версия | 2 |
| **Ori and the Will of the Wisps (2020)** | Unity 2018 (D3D11) | Нет античита; работает без лаунчера | 3 |
| **Portal 2 / Portal: Revolution** | Source (D3D9/11/Vulkan) | Без античита; сложный рендер | 3 |

**Серые зоны:** игры с Denuvo Anti‑Tamper (без kernel-античита) теоретически работают, но
требуют тестов (*Hades*, *Axiom Verge* могут её использовать — порту не мешает).

## 3. Магазины/лаунчеры (2026)
- **Steam** — лучшая совместимость; многие игры запускаются `.exe` напрямую без клиента.
- **GOG Galaxy** — клиент под Wine нестабилен, НО большинство игр DRM-free → запуск из папки. **Самый чистый путь для bring-up.**
- **Epic Games Launcher** — часто ломается (краш на обязат. обновлении, вход не работает). Отдельные игры DRM-free (`-EpicPortal`).
- **EA App / Ubisoft Connect / Rockstar** — требуют kernel-античит/свои сервисы → исключить.
- **Microsoft Store** — UWP-инфраструктура усложняет; отдельные игры (Ori) идут без Store.

## 4. Рекомендации
1. Избегать kernel-античитов (EAC Non‑EOS/BattlEye/Vanguard/Ricochet/GameGuard/PunkBuster).
2. Приоритет — D3D9/11/Vulkan/OpenGL; DX12-only (Elden Ring, Forza) — позже/сложнее.
3. **Первые 10 целей:** Hades, Stardew Valley, Hollow Knight, Celeste, Cuphead, The Talos
   Principle, Dead Cells, Hyper Light Drifter, Limbo, The Witness.
4. Сложные движки (Ori, Portal 2/Revolution, Inside, Obra Dinn, Axiom Verge) — резерв, тестировать.
5. Следить за обновлениями античита (EAC/BattlEye могут получить Proton по решению разработчика).

## 5. Открытые вопросы
- Производительность DX12 внутри GPTK ещё не ясна — тестировать Elden Ring / Plague Tale.
- Denuvo Anti‑Tamper: онлайн-проверки + аппаратная привязка под Wine/ARM64 — выяснить.
- Альтернативные загрузчики Epic/EA/Ubisoft (Heroic) — протестировать на macOS ARM.
- WoW64 vs ARM64EC для чистого x86-64 кода — режимы FEX/Rosetta исследовать раздельно.
