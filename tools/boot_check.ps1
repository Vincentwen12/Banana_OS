# Quick boot check: does bash reach readiness marker?
$stream = Join-Path $PSScriptRoot "..\boot_check.log"
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
        [System.IO.File]::AppendAllText("D:\BananaOS-Axion\boot_check.log", $EventArgs.Data + "`n")
    }
}
$p.BeginOutputReadLine()
$deadline = (Get-Date).AddSeconds(20)
$ready = $false; $crashed = $false
while ((Get-Date) -lt $deadline) {
    $c = Get-Content $stream -Raw -ErrorAction SilentlyContinue
    if ($c -match "initialize_job_control") { $ready = $true; break }
    if ($c -match "bash exited") { $crashed = $true }
    Start-Sleep -Milliseconds 400
}
$p.Kill(); $p.WaitForExit()
Unregister-Event -SourceIdentifier $handler.Name -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300
$c = Get-Content $stream -Raw
if ($c -match "user fault") { Write-Host "FAULT: user fault detected" }
if ($ready) { Write-Host "RESULT: BASH OK" } else { Write-Host "RESULT: BASH CRASH" }
$tail = $c.Substring([Math]::Max(0, $c.Length - 600))
Write-Host $tail
