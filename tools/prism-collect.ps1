# ПОЛНАЯ карта Prism - эмулятора x86/x64 в Windows 11 на ARM64.
#
# Зачем целиком, а не под текущий вопрос. Мы переписываем ровно тот интерфейс, который Prism
# реализует: наши модули называются xtajit.dll и xtajit64.dll не случайно. Всё, что он делает
# штатно, для нас - бесплатная истина, которую не надо добывать замерами. Собирать под одну
# текущую боль - значит через неделю обнаружить, что не хватает соседнего куска, и лезть снова.
# Поэтому здесь снимается ВСЁ, что можно снять чтением, а разделы помечены по ценности.
#
# Запуск в Windows-виртуалке:
#   powershell -ExecutionPolicy Bypass -File prism-collect.ps1 > PRISM-ОТЧЁТ.txt 2>&1
# Часть разделов полнее под администратором, но без него скрипт тоже отработает.
#
# Ничего не меняет. Только читает.

$ErrorActionPreference = "SilentlyContinue"
function Head($t) { "`n`n==== $t " + ("=" * [Math]::Max(0, 70 - $t.Length)) }
function Sub($t)  { "`n-- $t --" }

Head "0. СИСТЕМА"
"Архитектура процесса : $env:PROCESSOR_ARCHITECTURE"
"Архитектура железа   : $env:PROCESSOR_ARCHITEW6432"
$os = Get-CimInstance Win32_OperatingSystem
"ОС                   : $($os.Caption)  build $($os.BuildNumber).$((Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').UBR)"
"Процессор            : $((Get-CimInstance Win32_Processor).Name)"

Head "1. ВСЕ ДВОИЧНЫЕ ФАЙЛЫ ЭМУЛЯЦИИ - полный обход, а не список известных имён"
# Список известных имён пропустит то, чего мы не знаем. Поэтому ищем по каталогам целиком.
Sub "по имени (известные компоненты)"
$known = @("xtajit*","XtaCache*","wow64*","wowarm*","ARM64EC*","xtabase*","chpe*","soft*intrin*")
foreach ($dir in @("$env:SystemRoot\System32","$env:SystemRoot\SysArm32","$env:SystemRoot\SysWOW64")) {
    foreach ($pat in $known) {
        Get-ChildItem "$dir\$pat" -File 2>$null | ForEach-Object {
            "{0,-22} {1,12:N0}  v{2,-22} {3}" -f $_.Name, $_.Length, $_.VersionInfo.FileVersion, $dir
        }
    }
}
Sub "* ТАБЛИЦА ЭКСПОРТА xtajit64.dll - это и есть контракт, который мы реализуем"
# Самое ценное в разделе. Экспортируемые функции - буквальный список того, что от нашего
# xtajit64 ждёт загрузчик Windows. Сверив со своим, увидим ровно то, чего у нас нет.
$xta = "$env:SystemRoot\System32\xtajit64.dll"
if (Test-Path $xta) {
    if (Get-Command dumpbin -EA 0) { dumpbin /exports $xta }
    elseif (Get-Command llvm-readobj -EA 0) { llvm-readobj --coff-exports $xta }
    else {
        "Нет dumpbin/llvm-readobj. Разбор заголовка PE вручную:"
        # Минимальный разбор таблицы экспорта без внешних средств.
        $b = [IO.File]::ReadAllBytes($xta)
        $pe = [BitConverter]::ToInt32($b, 0x3C)
        $magic = [BitConverter]::ToUInt16($b, $pe + 24)
        $expDirOff = if ($magic -eq 0x20B) { $pe + 24 + 112 } else { $pe + 24 + 96 }
        $expRva = [BitConverter]::ToUInt32($b, $expDirOff)
        "  RVA таблицы экспорта: 0x{0:X}  (машина: 0x{1:X}, magic: 0x{2:X})" -f `
            $expRva, [BitConverter]::ToUInt16($b, $pe + 4), $magic
        "  Полный разбор - на стороне Mac, файл скопировать целиком (см. раздел 9)."
    }
}
Sub "зависимости xtajit64.dll"
if (Get-Command dumpbin -EA 0) { dumpbin /dependents $xta }

Head "2. КЕШ ТРАНСЛЯЦИЙ - где, что, каким именем, какого размера"
# У нас общий кеш сдвигал вход в GOG с 537-й секунды на 184-ю. Первоклассный рычаг, поэтому
# интересно всё: раскладка, именование, размер на модуль, срок жизни.
foreach ($c in @("$env:SystemRoot\XtaCache","$env:SystemRoot\System32\XtaCache",
                 "$env:LOCALAPPDATA\Microsoft\XtaCache","$env:ProgramData\Microsoft\XtaCache")) {
    if (Test-Path $c) {
        Sub "каталог $c"
        $items = Get-ChildItem $c -File -Recurse 2>$null
        "  файлов: $($items.Count), суммарно: {0:N1} МБ" -f (($items | Measure-Object Length -Sum).Sum / 1MB)
        "  по расширениям: " + (($items | Group-Object Extension |
            ForEach-Object { "$($_.Name)=$($_.Count)" }) -join ", ")
        "  крупнейшие 10:"
        $items | Sort-Object Length -Desc | Select-Object -First 10 |
            ForEach-Object { "    {0,12:N0}  {1}  {2}" -f $_.Length, $_.LastWriteTime.ToString("dd.MM HH:mm"), $_.Name }
        "  ПЕРВЫЕ БАЙТЫ крупнейшего файла (сигнатура формата):"
        $big = $items | Sort-Object Length -Desc | Select-Object -First 1
        if ($big) {
            $h = [IO.File]::ReadAllBytes($big.FullName)[0..63]
            "    " + (($h | ForEach-Object { "{0:X2}" -f $_ }) -join " ")
            "    ASCII: " + (($h | ForEach-Object { if ($_ -ge 32 -and $_ -lt 127) { [char]$_ } else { "." } }) -join "")
        }
        "  права доступа:"; (Get-Acl $c).Access | ForEach-Object { "    $($_.IdentityReference) : $($_.FileSystemRights)" }
    }
}

Head "3. СЛУЖБЫ, ДРАЙВЕРЫ, ЗАДАНИЯ"
Sub "службы"
Get-CimInstance Win32_Service | Where-Object { $_.Name -match "Xta|Prism|Emul" } |
    ForEach-Object { "{0,-16} {1,-9} запуск={2,-10} путь={3}" -f $_.Name, $_.State, $_.StartMode, $_.PathName }
Sub "драйверы"
Get-CimInstance Win32_SystemDriver | Where-Object { $_.Name -match "Xta|Prism|Emul|arm64" } |
    ForEach-Object { "{0,-16} {1,-9} {2}" -f $_.Name, $_.State, $_.PathName }
Sub "запланированные задания"
Get-ScheduledTask | Where-Object { $_.TaskName -match "Xta|Prism|Emul" } |
    ForEach-Object { "$($_.TaskPath)$($_.TaskName) - $($_.State)" }
Sub "живые процессы"
Get-Process | Where-Object { $_.Name -match "Xta|Prism" } |
    ForEach-Object { "{0} pid={1} набор={2:N1} МБ потоков={3}" -f $_.Name, $_.Id, ($_.WorkingSet64/1MB), $_.Threads.Count }

Head "4. РЕЕСТР - обход поддеревьев целиком, а не выборочные ключи"
foreach ($root in @("HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\XtaCache",
                    "HKLM:\SYSTEM\CurrentControlSet\Services\XtaCache",
                    "HKLM:\SOFTWARE\Microsoft\Wow64",
                    "HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management")) {
    if (Test-Path $root) {
        Sub $root
        Get-ChildItem $root -Recurse -EA 0 | ForEach-Object { "  подключ: $($_.Name)" }
        Get-ItemProperty $root | Format-List | Out-String
    }
}

Head "5. КАК ВЫГЛЯДИТ ЖИВОЙ ЭМУЛИРУЕМЫЙ ПРОЦЕСС"
# Ради этого раздела всё и затевалось изначально, но он теперь один из многих.
# У нас гостевые образы НЕ заведены в учёте виртуальной памяти: проба внутри UnityPlayer
# отдаёт alloc_base=0, MEM_FREE, PAGE_NOACCESS, хотя игра жива и грузит объекты. Здесь видно,
# как то же самое выглядит у Prism.
$emul = Get-Process | Where-Object {
    $_.Path -and $_.Path -notmatch "\\Windows\\" -and $_.Modules.Count -gt 3
} | Select-Object -First 3
if (-not $emul) {
    "НЕ НАЙДЕНО. Запусти x64-приложение (НЕ ARM64) и повтори. Разрядность видно в диспетчере"
    "задач: Подробности -> правый клик по заголовку -> столбец "Архитектура"."
} else {
    foreach ($p in $emul) {
        Sub "$($p.Name) pid=$($p.Id)"
        "  путь: $($p.Path)"
        "  потоков: $($p.Threads.Count), модулей: $($p.Modules.Count)"
        "  модули с базами и размерами - видны ли гостевые образы нормально:"
        $p.Modules | Select-Object -First 20 | ForEach-Object {
            "    0x{0:X16}  {1,10:N0}  {2}" -f $_.BaseAddress.ToInt64(), $_.ModuleMemorySize, $_.ModuleName
        }
    }
    "`n  ВЫВОД ДЛЯ НАС: если модули эмулируемого процесса перечислены с настоящими базами,"
    "  значит Prism заводит гостевые образы в учёте виртуальной памяти, и наш MEM_FREE -"
    "  это НАШ пропуск, а не неизбежная цена эмуляции."
}

Head "6. ТЕЛЕМЕТРИЯ И ЖУРНАЛЫ - чем Prism сам о себе сообщает"
Sub "поставщики ETW"
(logman query providers) -split "`n" | Where-Object { $_ -match "Xta|Prism|Wow64|Emul" }
Sub "журналы событий"
Get-WinEvent -ListLog * -EA 0 | Where-Object { $_.LogName -match "Xta|Prism|Emul" } |
    ForEach-Object { "$($_.LogName): записей $($_.RecordCount)" }
Sub "счётчики производительности"
(Get-Counter -ListSet * -EA 0 | Where-Object { $_.CounterSetName -match "Xta|Prism|Emul" }).CounterSetName

Head "7. ЗАПУСК ПОД ЭМУЛЯЦИЕЙ - как система решает, что образ надо транслировать"
Sub "поддержка архитектур образов"
Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager" -EA 0 |
    Select-Object * | Format-List | Out-String
Sub "ARM64EC и режимы совместимости"
Get-ChildItem "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags" -EA 0 |
    ForEach-Object { "  $($_.Name)" }

Head "8. САМОМОДИФИЦИРУЮЩИЙСЯ КОД - у нас Mono JIT-ит на ходу"
@"
  Чтением не снимается. Нужен запуск .NET-приложения под эмуляцией и наблюдение, как
  Prism переживает генерацию кода на ходу: перетранслирует ли, инвалидирует ли кеш,
  и появляются ли новые файлы в XtaCache во время работы.
  Порядок: запустить .NET-приложение, снять снимок XtaCache до и после, сравнить.
"@

Head "9. ЧТО НАДО ВЫНЕСТИ НА MAC ДЛЯ РАЗБОРА (чтением не берётся)"
@"
  1. Сами двоичные файлы: xtajit64.dll, xtajit.dll, XtaCache.exe, wow64.dll, wowarmhw.dll.
     Скопировать в общую папку - разберём на Mac таблицы экспорта и импорта целиком.
  2. Один-два файла кеша из XtaCache - для разбора формата.
  3. Трасса Process Monitor (procmon) при первом и повторном запуске одного приложения:
     покажет ПОРЯДОК обращений, чего никакой список файлов не даст.
     Фильтр по имени процесса, сохранить в .PML.
  4. Дамп памяти эмулируемого процесса, если получится, - покажет раскладку регионов.
"@

Head "КОНЕЦ"
