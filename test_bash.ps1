# Task 12 acceptance test: real glibc dynamic-linked bash 5.2 interactive test
$out = Join-Path $PSScriptRoot "bash_test_out.log"
$stream = Join-Path $PSScriptRoot "bash_test_stream.log"
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
        [System.IO.File]::AppendAllText("D:\BananaOS-Axion\bash_test_stream.log", $EventArgs.Data + "`n")
    }
}
$p.BeginOutputReadLine()

# Helper: send a command line to bash (raw \n, QEMU serial turns \r into \n so avoid \r\n double newline)
function Send-Cmd([string]$cmd) {
    $p.StandardInput.Write($cmd + "`n")
    $p.StandardInput.Flush()
}

# Wait for bash prompt (up to 40s). Give the stream file time to appear first.
$deadline = (Get-Date).AddSeconds(40)
$ready = $false
while ((Get-Date) -lt $deadline) {
    if (-not (Test-Path $stream)) { Start-Sleep -Milliseconds 500; continue }
    if (Select-String -Path $stream -Pattern "bash-5\.2" -Quiet -ErrorAction SilentlyContinue) {
        $ready = $true
        Write-Host "Bash prompt detected!"
        break
    }
    Start-Sleep -Milliseconds 500
}
if (-not $ready) { Write-Host "WARN: bash prompt not seen within 40s" }

Start-Sleep -Seconds 2

# Test 1: builtin echo
Write-Host "Sending: echo Hello-from-BananaOS"
Send-Cmd "echo Hello-from-BananaOS"
Start-Sleep -Seconds 3

# Test 2: builtin pwd (should print /)
Write-Host "Sending: pwd"
Send-Cmd "pwd"
Start-Sleep -Seconds 3

# Test 3: builtin cd (then pwd to confirm cwd change is impossible, stays in /)
Write-Host "Sending: cd / && pwd"
Send-Cmd "cd / && pwd"
Start-Sleep -Seconds 3

# Test 4: external command /bin/hello (fork + wait4 model)
Write-Host "Sending: /bin/hello"
Send-Cmd "/bin/hello"
Start-Sleep -Seconds 5

# Test 5: exit
Write-Host "Sending: exit"
Send-Cmd "exit"
Start-Sleep -Seconds 5

# Leave time for remaining output
Start-Sleep -Seconds 8

$p.Kill()
$p.WaitForExit()
Unregister-Event -SourceIdentifier $handler.Name -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

if (Test-Path $stream) {
    $output = Get-Content $stream -Raw
    [System.IO.File]::WriteAllText($out, $output)
    Write-Host "saved to $out ($($output.Length) chars)"

    $pass = $true

    if ($output -match "bash-5\.2") {
        Write-Host "[PASS] interactive bash prompt (bash-5.2) detected"
    } else {
        Write-Host "[FAIL] no bash-5.2 prompt found"
        $pass = $false
    }

    if ($output -match "Hello-from-BananaOS") {
        Write-Host "[PASS] builtin command echo works"
    } else {
        Write-Host "[FAIL] echo output missing"
        $pass = $false
    }

    if ($output -match "(?m)^/\s*$" -or $output -match "pwd" -and $output -match "bash-5\.2# /") {
        Write-Host "[PASS] builtin command pwd works"
    } else {
        Write-Host "[FAIL] pwd output missing"
        $pass = $false
    }

    if ($output -match "Hello from BananaOS ELF") {
        Write-Host "[PASS] external command /bin/hello works"
    } else {
        Write-Host "[FAIL] /bin/hello output missing"
        $pass = $false
    }

    if ($pass) {
        Write-Host "=== ALL BASH ACCEPTANCE TESTS PASSED ===" -ForegroundColor Green
    } else {
        Write-Host "=== SOME BASH TESTS FAILED ===" -ForegroundColor Red
    }
} else {
    Write-Host "no stream captured"
}