# W7 Phase 3.2: 512MB fs.img (4096B, 4 groups) + full toolchain smoke test.
# Boots disk.img + fs.img, waits for bash, then runs toolchain commands.
$ErrorActionPreference = "Continue"
$stream = Join-Path $PSScriptRoot "w7_fsimg_stream.log"
$out    = Join-Path $PSScriptRoot "w7_fsimg_out.log"
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
        [System.IO.File]::AppendAllText("D:\BananaOS-Axion\w7_fsimg_stream.log", $EventArgs.Data + "`n")
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

# let the one-time readline completion spam (empty-Enter trigger, ~100 lines)
# fully drain before sending any command, otherwise long input lines get
# interleaved with the spam and corrupted (e.g. "python3 -V" merged into "ls").
Start-Sleep -Seconds 30

# dismiss any pending "Display all N possibilities? (y or n)" prompt
Send-Cmd "n"
Start-Sleep -Seconds 2
Send-Cmd ""
Start-Sleep -Seconds 5

Write-Host "=== toolchain smoke ==="
Send-Cmd "ls /bin/bash /usr/bin/python3 /usr/bin/vim /usr/bin/gcc /usr/bin/make"
if (Wait-Match "make" 25 "ls toolchain paths") { $results += "ls:OK" } else { $results += "ls:FAIL" }

Send-Cmd "python3 -V"
if (Wait-Match "Python 3" 25 "python3 runs") { $results += "python3:OK" } else { $results += "python3:FAIL" }

Send-Cmd "vim --version"
if (Wait-Match "VIM - Vi IMproved" 25 "vim runs") { $results += "vim:OK" } else { $results += "vim:FAIL" }

Send-Cmd "cat /etc/passwd"
if (Wait-Match "banana:x:" 20 "passwd read") { $results += "passwd:OK" } else { $results += "passwd:FAIL" }

Send-Cmd "exit"
if (Wait-Match "bash exited" 20 "bash exited") { $results += "exit:OK" } else { $results += "exit:FAIL" }

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
    Write-Host "=== W7 FS.IMG TOOLCHAIN TEST PASSED ===" -ForegroundColor Green
    exit 0
} else {
    Write-Host "=== W7 FS.IMG TOOLCHAIN TEST FAILED ===" -ForegroundColor Red
    exit 1
}
