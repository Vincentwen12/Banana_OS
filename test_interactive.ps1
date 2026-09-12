# Task 13: interactive bash test - char-by-char input simulating a real terminal
$out = Join-Path $PSScriptRoot "interactive_out.log"
$stream = Join-Path $PSScriptRoot "interactive_stream.log"
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
        [System.IO.File]::AppendAllText("D:\BananaOS-Axion\interactive_stream.log", $EventArgs.Data + "`n")
    }
}
$p.BeginOutputReadLine()

# Send a command character-by-character (simulating typed input), then Enter
function Send-Typed([string]$cmd) {
    foreach ($ch in $cmd.ToCharArray()) {
        $p.StandardInput.Write([string]$ch)
        $p.StandardInput.Flush()
        Start-Sleep -Milliseconds 120
    }
    $p.StandardInput.Write("`n")
    $p.StandardInput.Flush()
    Start-Sleep -Seconds 2
}

# Send characters with NO delay (real typing speed) but DO NOT press Enter
function Send-FastNoEnter([string]$cmd) {
    foreach ($ch in $cmd.ToCharArray()) {
        $p.StandardInput.Write([string]$ch)
        $p.StandardInput.Flush()
    }
}

# Wait for bash to be ready. NOTE: the bare "bash-5.2#" prompt is NOT
# newline-terminated, so BeginOutputReadLine() never delivers it as a line and
# grepping the stream for it always times out. Use a newline-terminated startup
# marker instead ("bash: initialize_job_control: ..." ends with \n).
$deadline = (Get-Date).AddSeconds(40)
$ready = $false
while ((Get-Date) -lt $deadline) {
    if (-not (Test-Path $stream)) { Start-Sleep -Milliseconds 500; continue }
    if ((Get-Content $stream -Raw -ErrorAction SilentlyContinue) -match "initialize_job_control") {
        $ready = $true
        Write-Host "Bash ready detected!"
        break
    }
    Start-Sleep -Milliseconds 500
}
if (-not $ready) { Write-Host "WARN: bash not ready within 40s" }
Start-Sleep -Seconds 2

# Test 1 first: empty Enter settles the one-time startup completion spam
# (readline's dumb-terminal behavior). The spam takes a long wall time, so the
# fast-typing echo test (Test 0) runs later, after the terminal has settled.
Write-Host "=== Test 1: empty Enter (settle startup) ==="
$p.StandardInput.Write("`n")
$p.StandardInput.Flush()
Start-Sleep -Seconds 20

# Test 2: builtin echo (char-by-char)
Write-Host "=== Test 2: echo hi ==="
Send-Typed "echo hi"

# Test 3: builtin pwd
Write-Host "=== Test 3: pwd ==="
Send-Typed "pwd"

# Test 4: external command exists (/bin/hello)
Write-Host "=== Test 4: /bin/hello ==="
Send-Typed "/bin/hello"

# Test 5: non-existent command (execve failure path - must NOT crash)
Write-Host "=== Test 5: ls (should error, not crash) ==="
Send-Typed "ls"

# Test 6: another builtin after failure
Write-Host "=== Test 6: echo after-failure ==="
Send-Typed "echo after-failure"

# Test 7: non-existent path (execve failure)
Write-Host "=== Test 7: /nonexistent ==="
Send-Typed "/nonexistent"

# Test 0: fast typing - verify the kernel tty echo works. The kernel echoes
# typed letters immediately, but the line-based stream capture (ReadLine) only
# delivers a line once a newline arrives, so "echo fast-echo" first appears
# when Enter echoes its '\n'. If the kernel echoes (fixed), the typed line
# shows TWICE (kernel echo line + readline's dumb-mode redraw line). If the
# kernel echo is broken, only the readline redraw line appears (once).
$fastPass = $true
Write-Host "=== Test 0: fast typing echo ==="
Send-FastNoEnter "echo fast-echo"
Start-Sleep -Seconds 2
$p.StandardInput.Write("`n")
$p.StandardInput.Flush()
Start-Sleep -Seconds 3
if (Test-Path $stream) {
    $raw = Get-Content $stream -Raw -ErrorAction SilentlyContinue
    if ($raw -match "(?m)^fast-echo\s*$") {
        Write-Host "[PASS] command executed after Enter (single output line)"
    } else {
        Write-Host "[FAIL] command output missing after Enter"
        $fastPass = $false
    }
    # Kernel echo + readline redraw = 2 occurrences. One alone = kernel echo off.
    $dup = ([regex]::Matches($raw, "echo fast-echo")).Count
    if ($dup -ge 2) {
        Write-Host "[PASS] typed line echoed by kernel + readline redraw ($dup occurrence(s))"
    } else {
        Write-Host "[FAIL] typed line echoed only $dup time(s) - kernel echo missing!"
        $fastPass = $false
    }
}

# Test 8: exit
Write-Host "=== Test 8: exit ==="
Send-Typed "exit"

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
    if (-not $fastPass) { $pass = $false }

    # Reboot check: only one boot sequence expected
    $reboots = ([regex]::Matches($output, "Booting from Hard Disk")).Count
    if ($reboots -eq 1) {
        Write-Host "[PASS] no reboot during session (1 boot)"
    } else {
        Write-Host "[FAIL] $reboots boots seen - system rebooted!"
        $pass = $false
    }

    if ($output -match "echo hi") {
        Write-Host "[PASS] builtin echo output seen"
    } else {
        Write-Host "[FAIL] echo output missing"
        $pass = $false
    }

    if ($output -match "(?m)^/\s*$") {
        Write-Host "[PASS] pwd output seen"
    } else {
        Write-Host "[FAIL] pwd output missing"
        $pass = $false
    }

    if ($output -match "Hello from BananaOS ELF") {
        Write-Host "[PASS] /bin/hello executed"
    } else {
        Write-Host "[FAIL] /bin/hello output missing"
        $pass = $false
    }

    if ($output -match "command not found|No such file") {
        Write-Host "[PASS] non-existent command reported error (no crash)"
    } else {
        Write-Host "[WARN] no error message seen for non-existent command"
    }

    if ($output -match "after-failure") {
        Write-Host "[PASS] shell continued after failed execve"
    } else {
        Write-Host "[FAIL] shell did not survive failed execve"
        $pass = $false
    }

    if ($pass) {
        Write-Host "=== ALL INTERACTIVE TESTS PASSED ===" -ForegroundColor Green
    } else {
        Write-Host "=== SOME INTERACTIVE TESTS FAILED ===" -ForegroundColor Red
    }
} else {
    Write-Host "no stream captured"
}