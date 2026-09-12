# Wave 1 acceptance: 2.4/2.5/3.2/3.3/3.4 tests via bash + kernel shell
# ASCII-only comments (PowerShell 5.1 GBK pitfall).
$stream = Join-Path $PSScriptRoot "..\accept_wave1.log"
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
        [System.IO.File]::AppendAllText("D:\BananaOS-Axion\accept_wave1.log", $EventArgs.Data + "`n")
    }
}
$p.BeginOutputReadLine()

function Wait-Marker {
    param([string]$Pattern, [int]$TimeoutSec = 30)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $c = Get-Content $stream -Raw -ErrorAction SilentlyContinue
        if ($c -match $Pattern) { return $true }
        Start-Sleep -Milliseconds 400
    }
    return $false
}

Write-Host "waiting for bash ready..."
$ok = Wait-Marker "initialize_job_control" 45
if (-not $ok) { Write-Host "WARN: bash not ready"; }
Start-Sleep -Seconds 4   # let readline completion spam settle

Write-Host "== T1: /bin/test_fault (bash survives) =="
$p.StandardInput.WriteLine("/bin/test_fault"); $p.StandardInput.Flush()
Start-Sleep -Seconds 4

Write-Host "== T2: /bin/test_perm =="
$p.StandardInput.WriteLine("/bin/test_perm"); $p.StandardInput.Flush()
Start-Sleep -Seconds 4

Write-Host "== T3: /bin/test_sig =="
$p.StandardInput.WriteLine("/bin/test_sig"); $p.StandardInput.Flush()
Start-Sleep -Seconds 4

Write-Host "== T4: /bin/test_shm =="
$p.StandardInput.WriteLine("/bin/test_shm"); $p.StandardInput.Flush()
Start-Sleep -Seconds 4

Write-Host "== T5: /bin/test_tcp =="
$p.StandardInput.WriteLine("/bin/test_tcp"); $p.StandardInput.Flush()
Start-Sleep -Seconds 4

Write-Host "== T6: /bin/test_big w (1MB write, slow) =="
$p.StandardInput.WriteLine("/bin/test_big w"); $p.StandardInput.Flush()
Start-Sleep -Seconds 120

Write-Host "== exit bash -> kernel shell =="
$p.StandardInput.WriteLine("exit"); $p.StandardInput.Flush()
$ok = Wait-Marker "Type 'help'" 15
if (-not $ok) { Write-Host "WARN: kernel shell not seen" }
Start-Sleep -Seconds 2

Write-Host "== K1: run /bin/test_fault =="
$p.StandardInput.WriteLine("run /bin/test_fault"); $p.StandardInput.Flush()
Start-Sleep -Seconds 4

Write-Host "== K2: run /bin/test_perm =="
$p.StandardInput.WriteLine("run /bin/test_perm"); $p.StandardInput.Flush()
Start-Sleep -Seconds 4

Write-Host "== K3: run /bin/test_shm =="
$p.StandardInput.WriteLine("run /bin/test_shm"); $p.StandardInput.Flush()
Start-Sleep -Seconds 4

Start-Sleep -Seconds 5
$p.Kill()
$p.WaitForExit()
Unregister-Event -SourceIdentifier $handler.Name -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

$output = Get-Content $stream -Raw
$pass = $true
function Check {
    param([string]$Name, [string]$Pattern)
    if ($output -match $Pattern) {
        Write-Host "[PASS] $Name"
    } else {
        Write-Host "[FAIL] $Name"
        $script:pass = $false
    }
}
Check "test_fault in bash" "fault test start"
Check "test_perm in bash" "perm-test PASS"
Check "test_sig handler" "sig-handler-ok"
Check "test_shm" "shm-test PASS"
Check "test_tcp" "tcp-test PASS"
Check "test_big w" "big-test WROTE"
Check "test_fault in shell" "fault test start"
Check "test_perm in shell" "perm-test PASS"
Check "test_shm in shell" "shm-test PASS"

if ($pass) { Write-Host "=== WAVE1 ALL PASSED ===" -ForegroundColor Green }
else { Write-Host "=== WAVE1 SOME FAILED ===" -ForegroundColor Red }
