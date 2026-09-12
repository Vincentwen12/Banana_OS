# W7 Phase 3.1: multi-group EXT2 kernel driver smoke test.
# Boots with fs_multig.img (64MB, 4096B blocks, 2 groups). The 32MB padding
# file fills group 0's data region, forcing bash/libc/passwd blocks into
# group 1 -- so a successful bash + file read exercises ext2_read_inode
# (group 0) + ext2_data_block/ext2_read_block (group 1).
$ErrorActionPreference = "Continue"
$stream = Join-Path $PSScriptRoot "multig_stream.log"
$out    = Join-Path $PSScriptRoot "multig_out.log"
Remove-Item $stream -ErrorAction SilentlyContinue

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = "D:\qemu\qemu-system-x86_64.exe"
$psi.Arguments = "-drive format=raw,file=disk.img -drive format=raw,file=fs_multig.img -m 2G -nographic -no-shutdown -smp 4,sockets=1,cores=4,threads=1 -machine pc,accel=tcg"
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true

$p = [System.Diagnostics.Process]::Start($psi)
$handler = Register-ObjectEvent -InputObject $p -EventName OutputDataReceived -Action {
    if ($EventArgs.Data) {
        [System.IO.File]::AppendAllText("D:\BananaOS-Axion\multig_stream.log", $EventArgs.Data + "`n")
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

$results = @()
Write-Host "=== Boot, waiting for bash (data in group 1) ==="
if (Wait-Match "Running /bin/bash" 90 "bash started") { $results += "bash:OK" } else { $results += "bash:FAIL" }
$p.StandardInput.WriteLine("exit")
$p.StandardInput.Flush()
if (Wait-Match "bash exited" 20 "bash exited") { $results += "exit:OK" } else { $results += "exit:FAIL" }

Write-Host "=== cat /etc/passwd (file in group 1) ==="
$p.StandardInput.WriteLine("cat /etc/passwd")
$p.StandardInput.Flush()
if (Wait-Match "banana" 20 "passwd read") { $results += "passwd:OK" } else { $results += "passwd:FAIL" }

Write-Host "=== run /bin/test_perm (group 1) ==="
$p.StandardInput.WriteLine("run /bin/test_perm")
$p.StandardInput.Flush()
if (Wait-Match "perm" 20 "test_perm ran") { $results += "perm:OK" } else { $results += "perm:FAIL" }

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
    Write-Host "=== MULTI-GROUP EXT2 TEST PASSED ===" -ForegroundColor Green
    exit 0
} else {
    Write-Host "=== MULTI-GROUP EXT2 TEST FAILED ===" -ForegroundColor Red
    exit 1
}
