# 对照实验：不注入输入，观察 bash 是否阻塞在提示符
$out = Join-Path $PSScriptRoot "bash_test_none.log"
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = "D:\qemu\qemu-system-x86_64.exe"
$psi.Arguments = "-drive format=raw,file=disk.img -drive format=raw,file=fs.img -m 2G -nographic -no-shutdown -smp 4,sockets=1,cores=4,threads=1 -machine pc,accel=tcg"
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$p = [System.Diagnostics.Process]::Start($psi)

$stream = Join-Path $PSScriptRoot "bash_test_none_stream.log"
Remove-Item $stream -ErrorAction SilentlyContinue
$handler = Register-ObjectEvent -InputObject $p -EventName OutputDataReceived -Action {
    if ($EventArgs.Data) { Add-Content -Path "D:\BananaOS-Axion\bash_test_none_stream.log" -Value $EventArgs.Data }
}
$p.BeginOutputReadLine()

Start-Sleep -Seconds 25   # 不注入任何输入，观察 bash 行为
$p.Kill()
$p.WaitForExit()
Unregister-Event -SourceIdentifier $handler.Name -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
if (Test-Path $stream) {
    $output = Get-Content $stream -Raw
    [System.IO.File]::WriteAllText($out, $output)
    Write-Host "saved to $out ($($output.Length) chars)"
} else { Write-Host "no stream captured" }
