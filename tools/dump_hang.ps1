# dump_hang.ps1 - Capture full dumps of a wedged Sunshine stream and its virtual-display
# driver host, the moment an alt-tab freezes the stream. Uses the built-in comsvcs MiniDump
# (no procdump required). RUN ELEVATED.
#
#   powershell -ExecutionPolicy Bypass -File D:\sources\libvirtualdisplay\tools\dump_hang.ps1
#
# Dumps land in C:\CrashDumps. Grab them the instant the stream freezes, before doing anything
# else (the hung threads are the evidence).

$ErrorActionPreference = 'Continue'
$out = 'C:\CrashDumps'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'

function Dump-Proc([int]$ProcId, [string]$Tag) {
    $file = Join-Path $out ("{0}_{1}_{2}.dmp" -f $Tag, $ProcId, $stamp)
    Write-Host ("Dumping {0} (PID {1}) -> {2}" -f $Tag, $ProcId, $file)
    # rundll32 takes "<dll>,<entrypoint>" as a single token; comsvcs MiniDump args: <pid> <file> full
    & rundll32.exe "C:\Windows\System32\comsvcs.dll,MiniDump" $ProcId $file full
    if (Test-Path $file) {
        Write-Host ("  OK  {0:N1} MB" -f ((Get-Item $file).Length / 1MB))
    } else {
        Write-Host "  FAILED (need elevation / SeDebugPrivilege?)"
    }
}

# 1) The stream host.
$hosts = Get-Process sunshine, vibeshine -ErrorAction SilentlyContinue
if (-not $hosts) { Write-Host "WARNING: no sunshine/vibeshine process found." }
foreach ($p in $hosts) { Dump-Proc $p.Id $p.Name }

# 2) The WUDFHost that hosts the virtual-display driver (find it by loaded module).
$found = $false
try {
    $rows = tasklist /m SunshineVirtualDisplayDriver.dll /fo csv 2>$null | ConvertFrom-Csv
    foreach ($r in $rows) {
        if ($r.PID -match '^\d+$') { Dump-Proc ([int]$r.PID) 'WUDFHost_vdd'; $found = $true }
    }
} catch { }
if (-not $found) {
    Write-Host "Could not locate the VDD WUDFHost by module; dumping all WUDFHost processes as a fallback."
    foreach ($p in (Get-Process WUDFHost -ErrorAction SilentlyContinue)) { Dump-Proc $p.Id 'WUDFHost' }
}

Write-Host "Done. Dumps in $out"
