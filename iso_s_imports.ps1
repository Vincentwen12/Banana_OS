# Run 10x python3 -S importing heavy pure-py modules; detect intermittency.
$ErrorActionPreference = "Continue"
$stream = "D:\BananaOS-Axion\iso_s_stream.log"
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
        [System.IO.File]::AppendAllText("D:\BananaOS-Axion\iso_s_stream.log", $EventArgs.Data + "`n")
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
    # Feed in small chunks so QEMU's 16B 16550 FIFO never overflows (burst
    # writes drop bytes and corrupt long command lines).
    $full = $cmd + "`n"
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($full)
    for ($i = 0; $i -lt $bytes.Length; $i += 8) {
        $hi = [Math]::Min($i + 7, $bytes.Length - 1)
        $chunk = [System.Text.Encoding]::ASCII.GetString($bytes[$i..$hi])
        $p.StandardInput.Write($chunk)
        $p.StandardInput.Flush()
        Start-Sleep -Milliseconds 25
    }
}

$bootLog = Read-Log
if (-not (Wait-MatchAfter "Running /bin/bash" 90 "boot" $bootLog)) { $p.Kill(); exit 1 }
Start-Sleep -Seconds 5
Send-Line "n"
Start-Sleep -Seconds 2
Send-Line ""
Start-Sleep -Seconds 6

for ($i = 1; $i -le 10; $i++) {
    $snap = Read-Log
    Send-Line "python3 -S -c `"import os,json,urllib.request,urllib.parse,xml.etree.ElementTree,email,http.client;print('OK$i')`""
    $r = Wait-MatchAfter ("(?m)^OK" + $i + "$") 40 ("heavy import run $i") $snap
    Write-Host ("run " + $i + " : " + $r)
    if ($r -ne "ok") { break }
    Start-Sleep -Milliseconds 400
}

$p.Kill(); $p.WaitForExit()
Unregister-Event -SourceIdentifier $handler.Name -ErrorAction SilentlyContinue
