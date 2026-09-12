param([string]$Mode = "nographic")

$qemu = "D:\qemu\qemu-system-x86_64.exe"

if ($Mode -eq "stdio") {
    $args = "-drive format=raw,file=disk.img -m 2G -smp 1,sockets=1,cores=1,threads=1 -machine pc,accel=tcg -display none -serial stdio -monitor none -no-shutdown"
} else {
    $args = "-drive format=raw,file=disk.img -m 2G -nographic -smp 4,sockets=1,cores=4,threads=1 -machine pc,accel=tcg -no-shutdown"
}

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $qemu
$psi.Arguments = $args
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true

$p = [System.Diagnostics.Process]::Start($psi)

Start-Sleep -Seconds 4
$p.StandardInput.WriteLine("help")
Start-Sleep -Seconds 2
$p.StandardInput.WriteLine("hello")
Start-Sleep -Seconds 1
$p.StandardInput.WriteLine("echo banana")
Start-Sleep -Seconds 2

if (-not $p.HasExited) { $p.Kill(); $p.WaitForExit() }

$out = $p.StandardOutput.ReadToEnd()
$err = $p.StandardError.ReadToEnd()

"=== MODE: $Mode ===" | Write-Output
$out | Write-Output
"" | Write-Output
"--- STDERR ---" | Write-Output
$err | Write-Output