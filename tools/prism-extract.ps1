# Выгрузка ВСЕХ файлов Prism в общую папку для разбора на маке.
#
# Зачем отдельно от prism-collect.ps1. Тот снимает КАРТУ — что есть, где лежит, как настроено.
# Этот вытаскивает САМИ ФАЙЛЫ, потому что таблицы экспорта, импорта и секции читаются только из
# двоичного содержимого, а в PowerShell без dumpbin их не разобрать. Разбираем на маке
# скриптом tools/prism-parse.py — там есть чем.
#
# Что забирает: всё семейство трансляторов (их ПЯТЬ, а не один: xtajit, xtajit64, xtajit64se,
# xtajitf, xtajitse), слой wow64, службу кеша и образцы самого кеша.
#
# Открытие: игра грузит xtajit64se.dll, а мы реализуем xtajit64.dll — разные модули, разница
# 425 КБ. Пока не сверим таблицы экспорта обоих, мы не знаем, тот ли контракт воспроизводим.
#
# Запуск (лучше от администратора — часть файлов иначе не прочитается):
#   powershell -ExecutionPolicy Bypass -File prism-extract.ps1 -Dest "\\Mac\Home\Documents\MacRunner\Main\MacRunner\reports\prism\файлы"

param(
    [string]$Dest = "$env:USERPROFILE\Desktop\prism-файлы"
)

$ErrorActionPreference = "SilentlyContinue"
New-Item -ItemType Directory -Force -Path $Dest | Out-Null
$log = Join-Path $Dest "ВЫГРУЗКА.txt"
"Выгрузка $(Get-Date -Format 'yyyy-MM-dd HH:mm')" | Out-File $log -Encoding utf8

function Take($path, $why) {
    if (-not (Test-Path $path)) { return }
    $f = Get-Item $path
    $to = Join-Path $Dest $f.Name
    try {
        Copy-Item $path $to -Force -EA Stop
        $h = (Get-FileHash $to -Algorithm SHA256).Hash.Substring(0,16)
        $line = "{0,-24} {1,12:N0}  v{2,-30} {3}  [{4}]" -f `
                $f.Name, $f.Length, $f.VersionInfo.FileVersion, $h, $why
    } catch {
        $line = "{0,-24} НЕ СКОПИРОВАН: {1}" -f $f.Name, $_.Exception.Message
    }
    $line | Tee-Object -FilePath $log -Append
}

"--- семейство трансляторов (ядро того, что мы переписываем) ---" | Tee-Object $log -Append
foreach ($n in @("xtajit.dll","xtajit64.dll","xtajit64se.dll","xtajitf.dll","xtajitse.dll")) {
    Take "$env:SystemRoot\System32\$n" "транслятор"
}
"--- слой wow64 и служба кеша ---" | Tee-Object $log -Append
foreach ($n in @("wow64.dll","wow64base.dll","wow64con.dll","wow64win.dll","wow64cpu.dll",
                 "wowarmhw.dll","XtaCache.exe","xtabase.dll")) {
    Take "$env:SystemRoot\System32\$n" "инфраструктура"
}
"--- ARM64EC и всё прочее по маске (то, чего мы могли не знать) ---" | Tee-Object $log -Append
Get-ChildItem "$env:SystemRoot\System32\*" -Include "*arm64ec*","*chpe*","*xta*" -File |
    ForEach-Object { Take $_.FullName "по маске" }

"--- образцы кеша трансляций ---" | Tee-Object $log -Append
# Кеш может быть пуст, пока эмулируемое приложение не поработало. Если пусто — запустить
# x64-приложение (например Hollow Knight) и повторить: интересен именно формат файлов.
$cacheDir = Join-Path $Dest "кеш"
New-Item -ItemType Directory -Force -Path $cacheDir | Out-Null
foreach ($c in @("$env:SystemRoot\XtaCache","$env:LOCALAPPDATA\Microsoft\XtaCache",
                 "$env:ProgramData\Microsoft\XtaCache")) {
    if (-not (Test-Path $c)) { continue }
    $items = Get-ChildItem $c -File -Recurse | Sort-Object Length -Descending
    "  $c : файлов $($items.Count)" | Tee-Object $log -Append
    # Берём три крупнейших — для разбора формата этого достаточно, а весь кеш тащить незачем.
    $items | Select-Object -First 3 | ForEach-Object {
        Copy-Item $_.FullName (Join-Path $cacheDir $_.Name) -Force
        "    взят: {0} ({1:N0} байт)" -f $_.Name, $_.Length | Tee-Object $log -Append
    }
}

"" | Tee-Object $log -Append
"ИТОГО в $Dest : $((Get-ChildItem $Dest -Recurse -File).Count) файлов, {0:N1} МБ" -f `
    (((Get-ChildItem $Dest -Recurse -File) | Measure-Object Length -Sum).Sum / 1MB) |
    Tee-Object $log -Append
"Дальше: на маке запустить tools/prism-parse.py по этой папке." | Tee-Object $log -Append
