# Phase 2.1 test: /bin/test_pipe (pipe/readv/poll/getdents64)
$ErrorActionPreference = "Continue"
$stream = Join-Path $PSScriptRoot "pipe_stream.log"
$out    = Join-Path $PSScriptRoot "pipe_out.log"
Remove-Item $stream -ErrorAction SilentlyContinue

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = "D:\qemu\qemu-system-x86_64.exe"
$psi.Arguments = "-drive format=raw,file=disk.img -drive format=raw,file=fs.img -m 2G -nographic -no-shutdown -smp 4,sockets=1,cores=4,threads=1 -machine pc,accel=tcg"
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true

$p = [System.Diagnostics.Process]::Start($psi)
$handler = Register-ObjectEvent -InputObject $p -EventName OutputDataReceived -Action {
    if ($EventArgs.Data) {
        [System.IO.File]::AppendAllText("D:\BananaOS-Axion\pipe_stream.log", $EventArgs.Data + "`n")
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
function Wait-Match($pattern, $secs, $label) {
    $deadline = (Get-Date).AddSeconds($secs)
    while ((Get-Date) -lt $deadline) {
        if ((Read-Log) -match $pattern) { Write-Host "[OK] $label"; return $true }
        Start-Sleep -Milliseconds 400
    }
    Write-Host "[TIMEOUT] $label (waiting for: $pattern)"
    return $false
}

Write-Host "=== Boot, waiting for bash ==="
Wait-Match "Running /bin/bash" 60 "bash started"
$p.StandardInput.WriteLine("echo READY_OK")
$p.StandardInput.Flush()
Wait-Match "READY_OK" 40 "bash ready"
Start-Sleep -Seconds 5   # readline completion spam

# exit bash -> kernel shell, then run test_pipe from the kernel shell
Write-Host "=== exit bash ==="
$p.StandardInput.WriteLine("exit")
$p.StandardInput.Flush()
Wait-Match "bash exited, back at kernel shell" 20 "kernel shell"

Write-Host "=== run /bin/test_pipe ==="
$p.StandardInput.WriteLine("run /bin/test_pipe")
$p.StandardInput.Flush()
# 轮询等待完成标记（TCG 下较慢），而非固定 sleep
$done = Wait-Match "pipe test (PASS|FAIL)" 30 "pipe test finished"
$c = Read-Log

$pass = $true
if (-not ($c -match "pipe test PASS")) {
    Write-Host "[FAIL] no 'pipe test PASS'"
    $pass = $false
}
foreach ($m in @("pipe-ok", "readv-ok", "poll-ok", "getdents-ok")) {
    if ($c -match [regex]::Escape($m)) {
        Write-Host "[OK] found: $m"
    } else {
        Write-Host "[FAIL] missing: $m"
        $pass = $false
    }
}

Start-Sleep -Seconds 2
$p.Kill()
$p.WaitForExit()
Unregister-Event -SourceIdentifier $handler.Name -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
[System.IO.File]::WriteAllText($out, (Read-Log))
Write-Host "saved to $out"

if ($pass) {
    Write-Host "=== PIPE TEST PASSED ===" -ForegroundColor Green
    exit 0
} else {
    Write-Host "=== PIPE TEST FAILED ===" -ForegroundColor Red
    exit 1
}
