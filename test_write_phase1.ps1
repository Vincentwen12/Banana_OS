# Task 1.8/1.9 acceptance - Phase 1: alloc inode/block, overwrite /bin/hello data,
# grow file into single-indirect block. Then quit QEMU (disk persists in fs.img).
$ErrorActionPreference = "Stop"
$stream = Join-Path $PSScriptRoot "write_phase1.log"
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
        [System.IO.File]::AppendAllText("D:\BananaOS-Axion\write_phase1.log", $EventArgs.Data + "`n")
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

# boot -> bash -> kernel shell
Wait-For "bash-5\.2" 120 "bash prompt"
Send-Command "exit"
Wait-For "Type 'help'" 60 "kernel shell"

# Baseline content
Send-Command "cat /etc/hostname"
Start-Sleep -Seconds 3

# 1.8: alloc inode + block (prints free counts before/after)
Send-Command "fsalloc"
Start-Sleep -Seconds 3

# 1.9: overwrite direct block
Send-Command "wfile /etc/hostname 0 BANANA-OS-WRITE!"
Start-Sleep -Seconds 3
Send-Command "cat /etc/hostname"
Start-Sleep -Seconds 3

# 1.9: grow into single-indirect block (offset 12288 = block 12)
Send-Command "wfile /etc/hostname 12288 HELLO-INDIRECT"
Start-Sleep -Seconds 3

Start-Sleep -Seconds 5
$p.Kill()
$p.WaitForExit()
Unregister-Event -SourceIdentifier $handler.Name -ErrorAction SilentlyContinue
Write-Host "Phase1 done, log saved to $stream"
