# Reproduce python3 REPL crash with syscall instrumentation.
$ErrorActionPreference = "Continue"
$stream = Join-Path $PSScriptRoot "pyrepl_stream.log"
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
        [System.IO.File]::AppendAllText("D:\BananaOS-Axion\pyrepl_stream.log", $EventArgs.Data + "`n")
    }
}
$p.BeginOutputReadLine()
function Read-Log { for ($i=0;$i -lt 5;$i++){ try{ return [System.IO.File]::ReadAllText($stream) }catch{ Start-Sleep -Milliseconds 200 } }; return "" }
function Wait-Match($pat,$secs,$label){ $dl=(Get-Date).AddSeconds($secs); while((Get-Date) -lt $dl){ if((Read-Log) -match $pat){ Write-Host "[OK] $label"; return }; Start-Sleep -Milliseconds 300 }; Write-Host "[TIMEOUT] $label ($pat)" }
function Send-Cmd($cmd){ $p.StandardInput.WriteLine($cmd); $p.StandardInput.Flush() }

Wait-Match "Running /bin/bash" 180 "bash started"
Start-Sleep -Seconds 5
Send-Cmd "n"; Start-Sleep -Seconds 2
Send-Cmd "";  Start-Sleep -Seconds 5
Send-Cmd "python -V"
Wait-Match "Python 3" 30 "first python -V"
Start-Sleep -Seconds 1
Send-Cmd "vim --version"
Wait-Match "VIM - Vi IMproved" 30 "vim runs"
Start-Sleep -Seconds 8
Send-Cmd "python3"
Start-Sleep -Seconds 8
Send-Cmd "print(1+1)"
Wait-Match "Fatal Python error|init_sys_streams|>>>" 20 "python3 started or crashed"
Start-Sleep -Seconds 3
$p.Kill(); $p.WaitForExit()
Unregister-Event -SourceIdentifier $handler.Name -ErrorAction SilentlyContinue
Write-Host "=== done ==="
