# Сбор эталонных данных о Prism — эмуляторе x86/x64 в Windows 11 на ARM64.
#
# Зачем. Мы переписываем ровно тот интерфейс, который Prism реализует: наши модули называются
# xtajit.dll и xtajit64.dll не случайно. Всё, что Prism делает штатно, для нас — бесплатная
# истина, которую не надо добывать замерами.
#
# Главный вопрос, ради которого всё. У нас гостевые образы НЕ заведены в учёте виртуальной памяти
# Windows-стороны: проба по адресу внутри UnityPlayer отдаёт alloc_base=0, state=MEM_FREE,
# protect=PAGE_NOACCESS — при том что игра в этот момент жива и объекты грузит. Ни список
# загрузчика, ни опрос отображений их не видят, две независимые проверки слепы одинаково.
# Раздел 5 ниже показывает, КАК ЭТО ВЫГЛЯДИТ У PRISM на живом x64-процессе. Если у него тот же
# адрес отдаёт нормальный регион с базой и правами — значит он образы регистрирует, и надо
# смотреть чем. Это и есть ответ на нашу стену.
#
# Запуск в Windows-виртуалке (PowerShell, обычные права хватает почти везде):
#   powershell -ExecutionPolicy Bypass -File prism-collect.ps1 > PRISM-ОТЧЁТ.txt
#
# Ничего не меняет, только читает.

$ErrorActionPreference = "SilentlyContinue"

function Head($t) { "`n=== $t ===" }

Head "0. ЧТО ЗА СИСТЕМА"
"Архитектура ОС      : $env:PROCESSOR_ARCHITECTURE"
"Версия              : $((Get-CimInstance Win32_OperatingSystem).Caption) build $((Get-CimInstance Win32_OperatingSystem).BuildNumber)"

Head "1. ДВОИЧНЫЕ ФАЙЛЫ PRISM"
# Те же имена, что у нас. Размер и версия говорят, сколько там на самом деле кода.
$bins = @("xtajit64.dll","xtajit.dll","XtaCache.exe","wow64.dll","wowarmhw.dll","wow64cpu.dll",
          "wow64win.dll","xtabase.dll","ARM64EC.dll")
foreach ($b in $bins) {
    foreach ($dir in @("$env:SystemRoot\System32","$env:SystemRoot\SysArm32","$env:SystemRoot\SysWOW64")) {
        $p = Join-Path $dir $b
        if (Test-Path $p) {
            $f = Get-Item $p
            $v = (Get-Item $p).VersionInfo.FileVersion
            "{0,-16} {1,10:N0} байт  v{2}  {3}" -f $b, $f.Length, $v, $dir
        }
    }
}

Head "2. КЕШ ТРАНСЛЯЦИЙ — ЧТО И ГДЕ ХРАНИТСЯ"
# Наш собственный кеш трансляций сдвигал вход в GOG с 537-й секунды на 184-ю, то есть это
# первоклассный рычаг. Интересно ВСЁ: где лежит, чем именованы файлы, каков размер на модуль.
foreach ($c in @("$env:SystemRoot\XtaCache","$env:SystemRoot\System32\XtaCache",
                 "$env:LOCALAPPDATA\Microsoft\XtaCache")) {
    if (Test-Path $c) {
        "Каталог: $c"
        $items = Get-ChildItem $c -File -Recurse
        "  файлов: $($items.Count), суммарно: {0:N1} МБ" -f (($items | Measure-Object Length -Sum).Sum / 1MB)
        "  расширения: " + (($items | Group-Object Extension | ForEach-Object { "$($_.Name)=$($_.Count)" }) -join ", ")
        "  примеры имён (первые 8):"
        $items | Select-Object -First 8 | ForEach-Object { "    {0,12:N0}  {1}" -f $_.Length, $_.Name }
    }
}

Head "3. СЛУЖБЫ И ПРОЦЕССЫ ЭМУЛЯЦИИ"
Get-Service | Where-Object { $_.Name -match "Xta|Prism|Emulat" } |
    ForEach-Object { "{0,-20} {1,-10} {2}" -f $_.Name, $_.Status, $_.DisplayName }
Get-Process | Where-Object { $_.Name -match "Xta|Prism" } |
    ForEach-Object { "процесс: {0} pid={1} рабочий набор={2:N1} МБ" -f $_.Name, $_.Id, ($_.WorkingSet64/1MB) }

Head "4. НАСТРОЙКИ В РЕЕСТРЕ"
foreach ($k in @("HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\XtaCache",
                 "HKLM:\SYSTEM\CurrentControlSet\Services\XtaCache",
                 "HKLM:\SOFTWARE\Microsoft\Wow64")) {
    if (Test-Path $k) { "Ключ: $k"; Get-ItemProperty $k | Format-List | Out-String }
}

Head "5. ★ ГЛАВНОЕ: КАК PRISM УЧИТЫВАЕТ ПАМЯТЬ ЖИВОГО x64-ПРОЦЕССА"
# Здесь и лежит ответ на нашу стену. Нужен ЗАПУЩЕННЫЙ x64-процесс под эмуляцией.
$x64 = Get-Process | Where-Object {
    $_.Path -and $_.Modules.Count -gt 0 -and $_.Name -notmatch "^(System|Idle)$"
} | ForEach-Object {
    $m = $_.MainModule
    if ($m -and $m.FileName -match "\\Program Files\\|\\Users\\") { $_ }
} | Select-Object -First 3

if (-not $x64) {
    "НЕ НАЙДЕНО подходящего процесса. Запусти любое x64-приложение (не ARM64!) и повтори."
    "Проверить разрядность: диспетчер задач -> Подробности -> столбец «Архитектура»."
} else {
    foreach ($p in $x64) {
        "`n--- процесс $($p.Name) pid=$($p.Id) ---"
        "  путь: $($p.MainModule.FileName)"
        "  модулей в списке загрузчика: $($p.Modules.Count)"
        "  первые модули (база, размер, имя) — ВАЖНО: видны ли они с нормальной базой:"
        $p.Modules | Select-Object -First 12 | ForEach-Object {
            "    0x{0:X16}  {1,10:N0}  {2}" -f $_.BaseAddress.ToInt64(), $_.ModuleMemorySize, $_.ModuleName
        }
    }
    "`nЧТО ЭТО ЗНАЧИТ ДЛЯ НАС:"
    "  Если у эмулируемого x64-процесса модули перечислены с настоящими базами и размерами —"
    "  значит Prism заводит гостевые образы в учёте виртуальной памяти, и наши MEM_FREE/alloc_base=0"
    "  это НАШ пропуск, а не неизбежность эмуляции."
}

Head "6. ЧЕГО ЗДЕСЬ НЕТ И НУЖНО ДОБРАТЬ ОТДЕЛЬНО"
@"
  - Точный формат файлов кеша (.xtac и подобных) — нужен разбор двоичного содержимого.
  - Порядок обращений при первом запуске против повторного — нужен Process Monitor
    (procmon) с фильтром по имени процесса, сохранить в PML и посмотреть очерёдность.
  - Как Prism ведёт себя с самомодифицирующимся кодом — у нас Mono JIT-ит на ходу,
    и это отдельный вопрос, замеряется только запуском .NET-приложения под эмуляцией.
"@
