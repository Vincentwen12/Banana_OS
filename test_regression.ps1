# Task 12 regression test: run /bin/hello and run /bin/test_fork
$stream = Join-Path $PSScriptRoot "regression_stream.log"
$out = Join-Path $PSScriptRoot "regression_out.log"
Remove-Item $stream -ErrorAction SilentlyContinue

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = "D:\qemu\qemu-system-x86_64.exe"
$psi.Arguments = "-drive format=raw,file=disk.img -drive format=raw,file=fs.img -m 2G -nographic -no-shutdown -smp 4,sockets=1,cores=4,threads=1 -machine pc,accel=tcg"
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true

$p = [System.Diagnostics.Process]::Start($psi)

# Drain stdout asynchronously to file
$handler = Register-ObjectEvent -InputObject $p -EventName OutputDataReceived -Action {
    if ($EventArgs.Data) {
        [System.IO.File]::AppendAllText("D:\BananaOS-Axion\regression_stream.log", $EventArgs.Data + "`n")
    }
}
$p.BeginOutputReadLine()

# Wait for bash prompt (up to 30s)
$deadline = (Get-Date).AddSeconds(30)
$ready = $false
while ((Get-Date) -lt $deadline) {
    if (Select-String -Path $stream -Pattern "bash-5\.2" -Quiet -ErrorAction SilentlyContinue) {
        $ready = $true
        Write-Host "Bash prompt detected!"
        break
    }
    Start-Sleep -Milliseconds 500
}
if (-not $ready) { Write-Host "WARN: bash prompt not seen within 30s" }

Start-Sleep -Seconds 2

# Exit bash back to kernel shell
Write-Host "Sending: exit (return to kernel shell)"
$p.StandardInput.WriteLine("exit")
$p.StandardInput.Flush()
Start-Sleep -Seconds 3

# Wait for kernel shell prompt
$deadline = (Get-Date).AddSeconds(10)
$shell_ready = $false
while ((Get-Date) -lt $deadline) {
    if (Select-String -Path $stream -Pattern "Type 'help'" -Quiet -ErrorAction SilentlyContinue) {
        $shell_ready = $true
        Write-Host "Kernel shell prompt detected!"
        break
    }
    Start-Sleep -Milliseconds 500
}
if (-not $shell_ready) { Write-Host "WARN: kernel shell not seen" }

Start-Sleep -Seconds 1

# Test 1: run /bin/hello
Write-Host "=== Test 1: run /bin/hello ==="
$p.StandardInput.WriteLine("run /bin/hello")
$p.StandardInput.Flush()
Start-Sleep -Seconds 5

# Test 2: run /bin/test_fork
Write-Host "=== Test 2: run /bin/test_fork ==="
$p.StandardInput.WriteLine("run /bin/test_fork")
$p.StandardInput.Flush()
Start-Sleep -Seconds 5

# Leave time for remaining output
Start-Sleep -Seconds 5

$p.Kill()
$p.WaitForExit()
Unregister-Event -SourceIdentifier $handler.Name -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

if (Test-Path $stream) {
    $output = Get-Content $stream -Raw
    [System.IO.File]::WriteAllText($out, $output)
    Write-Host "saved to $out ($($output.Length) chars)"

    $pass = $true
    if ($output -match "Hello.*BananaOS|Hello.*world") {
        Write-Host "[PASS] run /bin/hello - Hello message detected"
    } else {
        Write-Host "[FAIL] run /bin/hello - No Hello message found"
        $pass = $false
    }
    if ($output -match "parent" -and $output -match "child" -and $output -match "reaped") {
        Write-Host "[PASS] run /bin/test_fork - fork/wait4/reap detected"
    } else {
        Write-Host "[FAIL] run /bin/test_fork - fork/wait4/reap not found"
        $pass = $false
    }
    if ($pass) {
        Write-Host "=== ALL REGRESSION TESTS PASSED ===" -ForegroundColor Green
    } else {
        Write-Host "=== SOME TESTS FAILED ===" -ForegroundColor Red
    }
} else {
    Write-Host "no stream captured"
}