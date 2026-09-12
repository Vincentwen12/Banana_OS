# W6 e2e acceptance: wave-1 tests (perm/sig/shm/tcp/big) + bash + job control
# Robust detection: [IO.File]::ReadAllText + retry (Get-Content races the
# concurrent AppendAllText writer and is locale-sensitive on GBK).
$ErrorActionPreference = "Continue"
$stream = Join-Path $PSScriptRoot "w6_stream.log"
$out    = Join-Path $PSScriptRoot "w6_out.log"
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
        [System.IO.File]::AppendAllText("D:\BananaOS-Axion\w6_stream.log", $EventArgs.Data + "`n")
    }
}
$p.BeginOutputReadLine()

function Read-Log {
    # Retry: the log is written by a background AppendAllText event, so a
    # single ReadAllText can race the writer and throw IOException. Retry
    # a few times before giving up (returns "" only after all attempts fail).
    for ($i = 0; $i -lt 5; $i++) {
        try {
            return [System.IO.File]::ReadAllText($stream)
        } catch {
            Start-Sleep -Milliseconds 200
        }
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

function Run-Test($cmd, $pattern, $label, $wait, $retries) {
    Write-Host "=== $label ==="
    $p.StandardInput.WriteLine($cmd)
    $p.StandardInput.Flush()
    $ok = $false
    for ($i = 0; $i -lt $retries; $i++) {
        Start-Sleep -Seconds $wait
        if ((Read-Log) -match $pattern) { $ok = $true; break }
    }
    if ($ok) {
        Write-Host "[OK] $label"
        $script:results += "$label`:OK"
    } else {
        Write-Host "[FAIL] $label (missing: $pattern)"
        $script:results += "$label`:FAIL"
    }
}

$results = @()

# 1. Boot + bash ready.
# NOTE: BeginOutputReadLine only dispatches \n-terminated lines; the bash
# prompt "bash-5.2#" has NO newline, so it never arrives alone. Use the
# \n-terminated "Running /bin/bash" marker, then ping bash with a probe
# command and wait for its echoed output to confirm it is actually ready.
Write-Host "=== Boot, waiting for bash ==="
$bash = Wait-Match "Running /bin/bash" 60 "bash started"
$p.StandardInput.WriteLine("echo READY_OK")
$p.StandardInput.Flush()
$bash2 = Wait-Match "READY_OK" 40 "bash ready"

# 2. Let readline completion spam settle, then check no job-control warning
Start-Sleep -Seconds 6
if ((Read-Log) -match "no job control") {
    Write-Host "[FAIL] bash printed 'no job control' warning"
    $results += "jobctrl:FAIL"
} else {
    Write-Host "[OK] no 'no job control' warning"
    $results += "jobctrl:OK"
}

# 3. bash echo hello
Write-Host "=== bash: echo hello ==="
$p.StandardInput.WriteLine("echo hello")
$p.StandardInput.Flush()
Start-Sleep -Seconds 3
if ((Read-Log) -match "hello") { $results += "bash_echo:OK" } else { $results += "bash_echo:FAIL" }

# 4. file persistence path (create/cat/rm inside bash)
Write-Host "=== bash: echo > /tmp/test.txt ; cat ; rm ==="
$p.StandardInput.WriteLine("echo ""persist"" > /tmp/test.txt")
$p.StandardInput.Flush()
Start-Sleep -Seconds 3
$p.StandardInput.WriteLine("cat /tmp/test.txt")
$p.StandardInput.Flush()
Start-Sleep -Seconds 3
$p.StandardInput.WriteLine("rm /tmp/test.txt")
$p.StandardInput.Flush()
Start-Sleep -Seconds 3
if ((Read-Log) -match "persist") { $results += "file_create_read:OK" } else { $results += "file_create_read:FAIL" }

# 5. exit bash -> kernel shell (use the unique post-exit marker, since the
# boot banner also contains "Type 'help'" at line 48)
Write-Host "=== exit bash ==="
$p.StandardInput.WriteLine("exit")
$p.StandardInput.Flush()
$shell = Wait-Match "bash exited, back at kernel shell" 20 "kernel shell prompt"

# 6. wave-1 acceptance tests (generous waits: TCG emulation is slow)
Run-Test "run /bin/test_perm"  "perm-test PASS"   "perm"       6 4
Run-Test "run /bin/test_sig"   "sig-handler-ok"   "sig"        6 4
Run-Test "run /bin/test_shm"   "shm-test PASS"    "shm"        8 4
Run-Test "run /bin/test_tcp"   "tcp-test PASS"    "tcp"        8 4
Run-Test "run /bin/test_big w" "big-test WROTE"   "big_write" 10 4
Run-Test "run /bin/test_big v" "big-test PASS"    "big_verify" 10 4

# 7. quick /proc /sys / ps
Run-Test "cat /proc/self/status" "Pid|Name" "proc_status" 4 4
Run-Test "cat /sys/kernel/version" "Axion|banana" "sys_version" 4 4
Run-Test "ps" "\d+" "ps_cmd" 4 4

# 8. job control: run /bin/sleep 2 & -> jobs lists it (busy-wait is slow
# under TCG, so poll until the job shows as Done)
Write-Host "=== job control: sleep & + jobs ==="
$p.StandardInput.WriteLine("run /bin/sleep 2 &")
$p.StandardInput.Flush()
Start-Sleep -Seconds 2
$p.StandardInput.WriteLine("jobs")
$p.StandardInput.Flush()
Start-Sleep -Seconds 1
$c = Read-Log
if ($c -match "Started job.*background" -and $c -match "sleep.*Done") {
    Write-Host "[OK] jobctl"
    $results += "jobctl:OK"
} else {
    $deadline = (Get-Date).AddSeconds(20)
    $jobok = $false
    while ((Get-Date) -lt $deadline) {
        $c = Read-Log
        if ($c -match "Started job.*background" -and $c -match "sleep.*Done") {
            $jobok = $true
            break
        }
        Start-Sleep -Milliseconds 1000
    }
    if ($jobok) {
        Write-Host "[OK] jobctl"
        $results += "jobctl:OK"
    } else {
        Write-Host "[FAIL] jobctl (no background job seen)"
        $results += "jobctl:FAIL"
    }
}

Start-Sleep -Seconds 2
$p.Kill()
$p.WaitForExit()
Unregister-Event -SourceIdentifier $handler.Name -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

if (Test-Path $stream) {
    [System.IO.File]::WriteAllText($out, (Read-Log))
    Write-Host "saved to $out"
}

Write-Host "=== SUMMARY ==="
$fail = 0
foreach ($r in $results) {
    Write-Host "  $r"
    if ($r -match ":FAIL") { $fail++ }
}
if ($fail -eq 0) {
    Write-Host "=== ALL W6 E2E TESTS PASSED ===" -ForegroundColor Green
} else {
    Write-Host "=== $fail TEST(S) FAILED ===" -ForegroundColor Red
}
exit $fail
