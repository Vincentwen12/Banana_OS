# Shell UX + toolchain crash repro: python alias, terminfo, no spam, tool cmds, bash survival.
$ErrorActionPreference = "Continue"
$stream = Join-Path $PSScriptRoot "shell_ux_stream.log"
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
        [System.IO.File]::AppendAllText("D:\BananaOS-Axion\shell_ux_stream.log", $EventArgs.Data + "`n")
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
        if ($tail -match $pattern) { Write-Host "[OK] $label"; return $true }
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
if (Wait-MatchAfter "Running /bin/bash" 180 "bash started" "") { $results += "bash:OK" } else { $results += "bash:FAIL"; $p.Kill(); exit 1 }
Start-Sleep -Seconds 5
Send-Cmd "n"; Start-Sleep -Seconds 2
Send-Cmd "";  Start-Sleep -Seconds 5

Send-Cmd "python -V"
if (Wait-MatchAfter "Python 3" 40 "python alias works" (Read-Log)) { $results += "python:OK" } else { $results += "python:FAIL" }
Start-Sleep -Seconds 1

Send-Cmd "ls /lib/terminfo/x/xterm"
if (Wait-MatchAfter "xterm" 30 "terminfo resolvable" (Read-Log)) { $results += "terminfo:OK" } else { $results += "terminfo:FAIL" }
Start-Sleep -Seconds 1

Send-Cmd "vim --version"
if (Wait-MatchAfter "VIM - Vi IMproved" 30 "vim runs" (Read-Log)) { $results += "vim:OK" } else { $results += "vim:FAIL" }
Start-Sleep -Seconds 1

Send-Cmd "gcc -v"
if (Wait-MatchAfter "gcc version" 30 "gcc runs" (Read-Log)) { $results += "gcc:OK" } else { $results += "gcc:FAIL" }
Start-Sleep -Seconds 1

Send-Cmd "make --version"
if (Wait-MatchAfter "GNU Make" 30 "make runs" (Read-Log)) { $results += "make:OK" } else { $results += "make:FAIL" }
Start-Sleep -Seconds 1

Send-Cmd "tar --version"
if (Wait-MatchAfter "tar \(" 30 "tar runs" (Read-Log)) { $results += "tar:OK" } else { $results += "tar:FAIL" }
Start-Sleep -Seconds 1

Send-Cmd "python3"
Start-Sleep -Seconds 6
Send-Cmd "print(1+1)"
if (Wait-MatchAfter "2" 30 "python3 REPL runs" (Read-Log)) { $results += "repl:OK" } else { $results += "repl:FAIL" }
Start-Sleep -Seconds 1
Send-Cmd "exit()"
Start-Sleep -Seconds 3

# bash survival: next command must produce output
Send-Cmd "ls /"
if (Wait-MatchAfter "home" 30 "bash alive after tools" (Read-Log)) { $results += "alive:OK" } else { $results += "alive:FAIL" }
Start-Sleep -Seconds 1

$p.Kill(); $p.WaitForExit()
Unregister-Event -SourceIdentifier $handler.Name -ErrorAction SilentlyContinue

$fail = 0
foreach ($r in $results) { Write-Host "  $r"; if ($r -match "FAIL") { $fail++ } }
if ($fail -eq 0) { Write-Host "=== SHELL UX TEST PASSED ==="; exit 0 }
else { Write-Host "=== SHELL UX TEST FAILED ==="; exit 1 }
