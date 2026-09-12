# W7 Phase 2.1+2.2 syscall tests: test_pipe + test_time from kernel shell
$ErrorActionPreference = "Continue"
$stream = Join-Path $PSScriptRoot "phase2_stream.log"
$out    = Join-Path $PSScriptRoot "phase2_out.log"
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
        [System.IO.File]::AppendAllText("D:\BananaOS-Axion\phase2_stream.log", $EventArgs.Data + "`n")
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
$p.StandardInput.WriteLine("exit")
$p.StandardInput.Flush()
Wait-Match "bash exited, back at kernel shell" 20 "kernel shell"

$results = @()
Write-Host "=== run /bin/test_pipe ==="
$p.StandardInput.WriteLine("run /bin/test_pipe")
$p.StandardInput.Flush()
if (Wait-Match "pipe test PASS" 30 "test_pipe done") { $results += "pipe:OK" } else { $results += "pipe:FAIL" }

Write-Host "=== run /bin/test_time ==="
$p.StandardInput.WriteLine("run /bin/test_time")
$p.StandardInput.Flush()
if (Wait-Match "time test PASS" 30 "test_time done") { $results += "time:OK" } else { $results += "time:FAIL" }

Start-Sleep -Seconds 2
$p.Kill()
$p.WaitForExit()
Unregister-Event -SourceIdentifier $handler.Name -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
[System.IO.File]::WriteAllText($out, (Read-Log))

$fail = 0
foreach ($r in $results) {
    Write-Host "  $r"
    if ($r -match "FAIL") { $fail++ }
}
if ($fail -eq 0) {
    Write-Host "=== PHASE 2 SYSCALL TESTS PASSED ===" -ForegroundColor Green
    exit 0
} else {
    Write-Host "=== PHASE 2 SYSCALL TESTS FAILED ===" -ForegroundColor Red
    exit 1
}
