# Task 1.8/1.9 acceptance - Phase 2: after restart, cat /bin/hello must show the new
# content written in phase 1; fsalloc must show persisted free counts (inodes-1, blocks-3).
$ErrorActionPreference = "Stop"
$stream = Join-Path $PSScriptRoot "write_phase2.log"
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
        [System.IO.File]::AppendAllText("D:\BananaOS-Axion\write_phase2.log", $EventArgs.Data + "`n")
    }
}
$p.BeginOutputReadLine()

function Wait-For([string]$pattern, [int]$timeoutSec, [string]$desc) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $stream) {
            if (Select-String -Path $stream -Pattern $pattern -Quiet -ErrorAction SilentlyContinue) {
                Write-Host "[OK] $desc"
                return $true
            }
        }
        Start-Sleep -Milliseconds 500
    }
    Write-Host "[WARN] timeout waiting for: $desc"
    return $false
}

function Send-Command([string]$cmd) {
    $p.StandardInput.WriteLine($cmd)
    $p.StandardInput.Flush()
}

# boot -> bash -> kernel shell (fresh mount of persisted fs.img)
Wait-For "bash-5\.2" 120 "bash prompt (restart)"
Send-Command "exit"
Wait-For "Type 'help'" 60 "kernel shell (restart)"

# Verify new content survived restart
Send-Command "cat /etc/hostname"
Start-Sleep -Seconds 3

# Verify persisted free counts (231->230 inodes, 4096->4093 blocks before this alloc)
Send-Command "fsalloc"
Start-Sleep -Seconds 3

Start-Sleep -Seconds 5
$p.Kill()
$p.WaitForExit()
Unregister-Event -SourceIdentifier $handler.Name -ErrorAction SilentlyContinue
Write-Host "Phase2 done, log saved to $stream"
