# Isolation: run N pure python3 processes (no imports) to test accumulation.
param([string]$Label = "pure")
$ErrorActionPreference = "Continue"
$stream = Join-Path $PSScriptRoot ("iso_" + $Label + "_stream.log")
Remove-Item $stream -ErrorAction SilentlyContinue

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = "D:\qemu\qemu-system-x86_64.exe"
$psi.Arguments = "-drive format=raw,file=disk.img -drive format=raw,file=fs.img -m 2G -nographic -no-shutdown -smp 1 -machine pc,accel=tcg"
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$p = [System.Diagnostics.Process]::Start($psi)
$handler = Register-ObjectEvent -InputObject $p -EventName OutputDataReceived -Action {
    if ($EventArgs.Data) {
        [System.IO.File]::AppendAllText("D:\BananaOS-Axion\iso_" + $Label + "_stream.log", $EventArgs.Data + "`n")
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
        if ($tail -match $pattern) { return "ok" }
        if ($tail -match "user fault|corrupt|Fatal Python|Traceback") { return "crash" }
        Start-Sleep -Milliseconds 300
    }
    return "timeout"
}
function Send-Line($cmd) {
    $p.StandardInput.WriteLine($cmd)
    $p.StandardInput.Flush()
}

$bootLog = Read-Log
if (-not (Wait-MatchAfter "Running /bin/bash" 90 "boot" $bootLog)) { $p.Kill(); exit 1 }
Start-Sleep -Seconds 5
Send-Line "n"
Start-Sleep -Seconds 2
Send-Line ""
Start-Sleep -Seconds 6

for ($i = 1; $i -le 15; $i++) {
    $snap = Read-Log
    Send-Line "python3 -c `"print(1)`""
    $r = Wait-MatchAfter "(?m)^1$" 20 ("pure run $i") $snap
    Write-Host ("run " + $i + " : " + $r)
    if ($r -ne "ok") { break }
    Start-Sleep -Milliseconds 500
}

$p.Kill(); $p.WaitForExit()
Unregister-Event -SourceIdentifier $handler.Name -ErrorAction SilentlyContinue
