# W7 Phase 3.2 long-timeout verification: python3/vim with generous timeouts.
$ErrorActionPreference = "Continue"
$stream = Join-Path $PSScriptRoot "w7_long_stream.log"
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
        [System.IO.File]::AppendAllText("D:\BananaOS-Axion\w7_long_stream.log", $EventArgs.Data + "`n")
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
function Send-Cmd($cmd) {
    $p.StandardInput.WriteLine($cmd)
    $p.StandardInput.Flush()
}

$results = @()
Write-Host "=== Boot, waiting for bash ==="
if (Wait-Match "Running /bin/bash" 90 "bash started") { $results += "bash:OK" } else { $results += "bash:FAIL" }

# let the one-time readline completion spam fully drain before sending commands
Start-Sleep -Seconds 15

Write-Host "=== python3 -V (long) ==="
Send-Cmd "python3 -V"
if (Wait-Match "Python 3" 180 "python3 runs") { $results += "python3:OK" } else { $results += "python3:FAIL" }

Write-Host "=== vim --version (long) ==="
Send-Cmd "vim --version"
if (Wait-Match "VIM - Vi IMproved" 180 "vim runs") { $results += "vim:OK" } else { $results += "vim:FAIL" }

Write-Host "=== exit ==="
Send-Cmd "exit"
if (Wait-Match "bash exited" 30 "bash exited") { $results += "exit:OK" } else { $results += "exit:FAIL" }

Start-Sleep -Seconds 2
$p.Kill()
$p.WaitForExit()
Unregister-Event -SourceIdentifier $handler.Name -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

$fail = 0
foreach ($r in $results) {
    Write-Host "  $r"
    if ($r -match "FAIL") { $fail++ }
}
if ($fail -eq 0) {
    Write-Host "=== W7 LONG TEST PASSED ==="
    exit 0
} else {
    Write-Host "=== W7 LONG TEST FAILED ==="
    exit 1
}
