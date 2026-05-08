# Фаза 0: Research & Setup

## Цель
Понять конкретно почему Civil 3D, Revit, Navisworks и 1С падают (или работают) на существующих Wine-решениях. Документировать каждую проблему.

## Тестовый план

### Программы для теста

| Программа | Источник установщика | Приоритет |
|-----------|---------------------|-----------|
| 1С Бухгалтерия 8.3 | пробная с 1c.ru | 🔥 Высокий |
| Notepad++ | notepad-plus-plus.org | Средний (sanity check) |
| AutoCAD LT 2024 | autodesk.com (trial) | 🔥 Высокий |
| AutoCAD Civil 3D 2024 | autodesk.com (trial) | 🔥 Высокий |
| Revit 2024 | autodesk.com (trial) | 🔥 Высокий |
| Navisworks Manage 2024 | autodesk.com (trial) | Высокий |
| КонсультантПлюс | consultant.ru | Средний |

### Окружения для теста

1. **CrossOver 26 trial** — baseline коммерческий
2. **Whisky (последняя)** — open source baseline
3. **Mythic** — для сравнения по UX
4. **Чистый Wine через brew** — для понимания "из коробки"

### Что замерять для каждой программы

```markdown
## Программа: <name>

### CrossOver
- Установка прошла: ✅/❌
- Запускается: ✅/❌
- Стабильна 30 минут: ✅/❌
- Конкретные ошибки: ...
- Логи: tests/logs/crossover-<name>.log

### Whisky
- (то же самое)

### Чистый Wine
- (то же самое)

### Что нужно чтобы заработало
- Отсутствующие DLL: ...
- Нужные патчи Wine: ...
- Лицензионные нюансы: ...
```

## Чеклист на эту неделю

### День 1: Окружение
- [ ] Запустить `./scripts/install-deps.sh`
- [ ] Скачать **CrossOver 26 trial** ([codeweavers.com](https://www.codeweavers.com/crossover/))
- [ ] Установить **Mythic** ([getmythic.app](https://getmythic.app/))
- [ ] **Whisky** ставится через `install-deps.sh`
- [ ] Запустить `./scripts/test-hello-world.sh` — проверить чистый Wine

### День 2-3: Сбор инсталляторов
- [ ] Зарегистрироваться на portal.autodesk.com — скачать trial Revit, Civil 3D, Navisworks
- [ ] Скачать 1С пробную (или платформу для разработчиков)
- [ ] Положить установщики в `tests/installers/` (gitignored)

### День 4-5: Тестирование 1С
- [ ] Установить 1С через CrossOver — заполнить отчёт
- [ ] Установить 1С через Whisky — заполнить отчёт
- [ ] Установить 1С через чистый Wine — заполнить отчёт
- [ ] Документировать все шаги в `docs/reports/1c-test.md`

### День 6-7: Тестирование Autodesk
- [ ] AutoCAD LT через все три
- [ ] Revit через все три
- [ ] Civil 3D через все три
- [ ] Navisworks через все три
- [ ] Документировать в `docs/reports/autodesk-test.md`

## После Фазы 0

Получим:
1. Реальную картину что работает / не работает
2. Конкретный список проблем для каждой программы
3. Понимание что чинить в нашем Wine форке
4. Базовые профили в `profiles/*.json`

Это и будет вход в Фазу 1.
