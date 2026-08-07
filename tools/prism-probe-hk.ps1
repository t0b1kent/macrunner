# Прицельный зонд: как Windows учитывает память ЭМУЛИРУЕМОГО x64-процесса.
#
# Это тот самый вопрос, на котором мы стоим. У нас проба по адресу внутри UnityPlayer отдаёт
# alloc_base=0, state=MEM_FREE, protect=PAGE_NOACCESS - при том что игра жива и грузит объекты.
# Ни список загрузчика, ни опрос отображений её образов не видят: две независимые проверки
# слепы одинаково. Значит гостевые образы не заведены в учёте виртуальной памяти.
#
# Здесь то же самое спрашивается у настоящего Prism, ТЕМ ЖЕ вызовом VirtualQueryEx. Если у него
# по адресу внутри UnityPlayer возвращается нормальный регион с базой и правами - наш MEM_FREE
# это НАШ пропуск, а не цена эмуляции, и чинить надо регистрацию образов.
#
# Прошлый заход провалился не по существу, а по отбору: фильтр "не из Windows" поймал GameBar и
# Edge, оба ARM64-нативные. Здесь процесс ищется по имени, а разрядность проверяется явно.
#
# Запуск (игра должна быть ЗАПУЩЕНА):
#   powershell -ExecutionPolicy Bypass -File prism-probe-hk.ps1 > ЗОНД-HK.txt 2>&1

$ErrorActionPreference = "SilentlyContinue"

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Mem {
    [StructLayout(LayoutKind.Sequential)]
    public struct MEMORY_BASIC_INFORMATION {
        public IntPtr BaseAddress;      public IntPtr AllocationBase;
        public uint   AllocationProtect; public IntPtr RegionSize;
        public uint   State;             public uint  Protect;  public uint Type;
    }
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern int VirtualQueryEx(IntPtr h, IntPtr addr,
        out MEMORY_BASIC_INFORMATION mbi, uint len);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool IsWow64Process2(IntPtr h, out ushort procMachine, out ushort nativeMachine);
}
"@

function StateName($s) { switch ($s) { 0x1000 {"MEM_COMMIT"} 0x2000 {"MEM_RESERVE"} 0x10000 {"MEM_FREE"} default {"0x{0:X}" -f $s} } }
function TypeName($t)  { switch ($t) { 0x20000 {"MEM_PRIVATE"} 0x40000 {"MEM_MAPPED"} 0x1000000 {"MEM_IMAGE"} default {"0x{0:X}" -f $t} } }
function ProtName($p)  {
    switch ($p) { 0x01 {"NOACCESS"} 0x02 {"READONLY"} 0x04 {"READWRITE"} 0x08 {"WRITECOPY"}
                  0x10 {"EXECUTE"} 0x20 {"EXECUTE_READ"} 0x40 {"EXECUTE_READWRITE"}
                  0x80 {"EXECUTE_WRITECOPY"} 0 {"-"} default {"0x{0:X}" -f $p} }
}

$targets = Get-Process | Where-Object { $_.Name -match "Hollow|hollow" }
if (-not $targets) {
    "Hollow Knight НЕ ЗАПУЩЕН. Запусти игру, дождись окна и повтори."
    "Если игра под другим именем - посмотри в диспетчере задач и подставь имя в фильтр."
    exit
}

foreach ($p in $targets) {
    "==== процесс $($p.Name) pid=$($p.Id) ===="
    "путь: $($p.Path)"

    # Разрядность и эмуляция - явно, а не по пути. Это и отличает эмулируемый процесс от нативного.
    $h = [Mem]::OpenProcess(0x1000 -bor 0x0010, $false, $p.Id)   # QUERY_LIMITED_INFORMATION | VM_READ
    if ($h -ne [IntPtr]::Zero) {
        $pm = 0; $nm = 0
        if ([Mem]::IsWow64Process2($h, [ref]$pm, [ref]$nm)) {
            $names = @{ 0x0 = "неизвестно/нативный"; 0x8664 = "x64"; 0x14c = "x86"; 0xAA64 = "ARM64"; 0x1c4 = "ARM" }
            "архитектура образа : {0} (0x{1:X})" -f $names[[int]$pm], $pm
            "архитектура железа : {0} (0x{1:X})" -f $names[[int]$nm], $nm
            if ($pm -eq 0x8664 -or $pm -eq 0x14c) { "*** ЭТО ЭМУЛИРУЕМЫЙ ПРОЦЕСС - то, что нужно ***" }
            else { "ВНИМАНИЕ: процесс НЕ эмулируется, показания к нашему вопросу не относятся." }
        }
    }

    "`n-- модули: видны ли гостевые образы в списке загрузчика --"
    "  всего модулей: $($p.Modules.Count)"
    $p.Modules | Select-Object -First 25 | ForEach-Object {
        "    0x{0:X16}  {1,10:N0}  {2}" -f $_.BaseAddress.ToInt64(), $_.ModuleMemorySize, $_.ModuleName
    }

    # ГЛАВНОЕ: спрашиваем про память ровно там, где у нас MEM_FREE - внутри образа игры.
    if ($h -ne [IntPtr]::Zero) {
        "`n-- * VirtualQueryEx по адресам ВНУТРИ образов (у нас здесь MEM_FREE) --"
        $probes = @()
        foreach ($m in ($p.Modules | Select-Object -First 6)) {
            # Смещение вглубь образа, а не его начало: начало могло бы отвечать и без регистрации.
            $probes += [PSCustomObject]@{
                Name = $m.ModuleName
                Addr = [IntPtr]($m.BaseAddress.ToInt64() + [Math]::Min(0x1000, $m.ModuleMemorySize / 2))
            }
        }
        foreach ($pr in $probes) {
            $mbi = New-Object Mem+MEMORY_BASIC_INFORMATION
            $n = [Mem]::VirtualQueryEx($h, $pr.Addr, [ref]$mbi, [Runtime.InteropServices.Marshal]::SizeOf($mbi))
            if ($n -gt 0) {
                "    {0,-24} адрес=0x{1:X12} база=0x{2:X12} размер={3,10:N0} {4,-12} {5,-10} {6}" -f `
                    $pr.Name, $pr.Addr.ToInt64(), $mbi.AllocationBase.ToInt64(), $mbi.RegionSize.ToInt64(),
                    (StateName $mbi.State), (TypeName $mbi.Type), (ProtName $mbi.Protect)
            } else {
                "    {0,-24} VirtualQueryEx НЕ ОТВЕТИЛ (ошибка {1})" -f $pr.Name, [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            }
        }
        "`n  ЧТО СРАВНИВАТЬ: у нас на таком же запросе - база=0, MEM_FREE, NOACCESS."
        "  Если выше стоит MEM_COMMIT + MEM_IMAGE с настоящей базой, значит Prism образы"
        "  регистрирует, и наш пропуск именно в этом."
    }
}
