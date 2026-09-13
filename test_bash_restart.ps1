# Phase A test: clean boot into bash + auto-restart after exit (login loop).
# Every fresh bash emits a readline completion question at startup; answer "n"
# first, then flush an empty line (proven recipe from test_shell_ux).
#
# Usage:    powershell -NoProfile -ExecutionPolicy Bypass -File .\test_bash_restart.ps1
# Requires: disk.img / fs.img built by build.ps1, plus QEMU.
# QEMU is resolved from $env:QEMU, then .\qemu-system-x86_64.exe, then the
# default D:\qemu\ path; set $env:QEMU to use your own installation.
$ErrorActionPreference = "Continue"
$stream = Join-Path $PSScriptRoot "bash_restart_stream.log"
Remove-Item $stream -ErrorAction SilentlyContinue

$Qemu = if ($env:QEMU) { $env:QEMU }
        elseif (Test-Path (Join-Path $PSScriptRoot "qemu-system-x86_64.exe")) { Join-Path $PSScriptRoot "qemu-system-x86_64.exe" }
        else { "D:\qemu\qemu-system-x86_64.exe" }
if (-not (Test-Path $Qemu)) {
    Write-Host "[ERROR] QEMU not found: $Qemu"
    Write-Host "        Set `$env:QEMU=<path to qemu-system-x86_64.exe> and retry."
    exit 2
}
$diskImg = Join-Path $PSScriptRoot "disk.img"
$fsImg   = Join-Path $PSScriptRoot "fs.img"
foreach ($img in @($diskImg, $fsImg)) {
    if (-not (Test-Path $img)) { Write-Host "[ERROR] missing $img - run .\build.ps1 first"; exit 2 }
}

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Qemu
$psi.Arguments = "-drive format=raw,file=`"$diskImg`" -drive format=raw,file=`"$fsImg`" -m 2G -nographic -no-shutdown -device rtl8139,netdev=n0 -netdev user,id=n0 -smp 1 -machine pc,accel=tcg"
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true

$p = [System.Diagnostics.Process]::Start($psi)
$handler = Register-ObjectEvent -InputObject $p -EventName OutputDataReceived -MessageData $stream -Action {
    if ($EventArgs.Data) {
        [System.IO.File]::AppendAllText($Event.MessageData, $EventArgs.Data + "`n")
    }
}
$p.BeginOutputReadLine()

function Read-Log {
    for ($i = 0; $i -lt 5; $i++) {
        try { return [System.IO.File]::ReadAllText($stream) }
        catch { Start-Sleep -Milliseconds 200 }
    }
    return ""
}
function Wait-MatchAfter($pattern, $secs, $label, $since) {
    $deadline = (Get-Date).AddSeconds($secs)
    while ((Get-Date) -lt $deadline) {
        $log = (Read-Log)
        $tail = ""
        if ($log.Length -gt $since.Length) { $tail = $log.Substring($since.Length) }
        if ($tail -match $pattern) { Write-Host "[OK] $label"; return $true }
        Start-Sleep -Milliseconds 400
    }
    Write-Host "[TIMEOUT] $label (waiting for: $pattern)"
    return $false
}
function Send-Line($cmd) {
    $p.StandardInput.WriteLine($cmd)
    $p.StandardInput.Flush()
}

# Settle a fresh bash: dismiss startup completion question, flush empty line.
function Settle-Bash {
    Start-Sleep -Seconds 5
    Send-Line "n"
    Start-Sleep -Seconds 2
    Send-Line ""
    Start-Sleep -Seconds 6
}

$results = @()
$bootLog = Read-Log
if (-not (Wait-MatchAfter "Running /bin/bash" 90 "boot into bash" $bootLog)) { $p.Kill(); Write-Host "=== RESTART TEST FAILED ==="; exit 1 }
$results += "boot:OK"

Settle-Bash

for ($i = 1; $i -le 2; $i++) {
    # Fresh bash must be functional: subprocess output visible.
    $snap = Read-Log
    Send-Line "ls /"
    if (Wait-MatchAfter "home" 30 "cycle $i bash responds to ls" $snap) { $results += "cycle${i}ls:OK" } else { $results += "cycle${i}ls:FAIL" }

    # Exit must respawn a fresh bash (kernel login loop).
    $snap = Read-Log
    Send-Line "exit"
    if (Wait-MatchAfter "Running /bin/bash" 60 "cycle $i bash auto-restarts" $snap) { $results += "cycle${i}restart:OK" } else { $results += "cycle${i}restart:FAIL"; break }

    Settle-Bash
}

# Final functional proof after the last restart.
$snap = Read-Log
Send-Line "ls /"
if (Wait-MatchAfter "home" 30 "final bash responds to ls" $snap) { $results += "finalls:OK" } else { $results += "finalls:FAIL" }

$p.Kill(); $p.WaitForExit()
Unregister-Event -SourceIdentifier $handler.Name -ErrorAction SilentlyContinue

$fail = 0
foreach ($r in $results) { Write-Host "  $r"; if ($r -match "FAIL") { $fail++ } }
if ($fail -eq 0) { Write-Host "=== BASH RESTART TEST PASSED ==="; exit 0 }
else { Write-Host "=== BASH RESTART TEST FAILED ==="; exit 1 }
