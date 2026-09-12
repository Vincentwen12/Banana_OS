# Verify: bare ls, second ls (no cd), cd + relative ls, cat relative, all via
# child-process output (bash's own stdout is buffered in readline dumb mode,
# so markers must come from children which flush at exit).
$ErrorActionPreference = "Continue"
$stream = Join-Path $PSScriptRoot "ls_cwd_stream.log"
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
        [System.IO.File]::AppendAllText("D:\BananaOS-Axion\ls_cwd_stream.log", $EventArgs.Data + "`n")
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

Write-Host "=== Boot, waiting for bash ==="
if (-not (Wait-MatchAfter "Running /bin/bash" 180 "bash started" "")) { $p.Kill(); exit 1 }
Start-Sleep -Seconds 30   # let one-time readline completion spam settle

Send-Cmd "n"
Start-Sleep -Seconds 2
Send-Cmd ""
Start-Sleep -Seconds 5

Write-Host "=== 1) first bare ls ==="
Send-Cmd "ls"
Wait-MatchAfter "\shome\s" 90 "ls #1 lists root" (Read-Log)
Start-Sleep -Seconds 2

Write-Host "=== 2) second bare ls (NO cd) ==="
Send-Cmd "ls"
Wait-MatchAfter "\shome\s" 90 "ls #2 lists root" (Read-Log)
Start-Sleep -Seconds 2

Write-Host "=== 3) cd /usr/bin ; ls ==="
Send-Cmd "cd /usr/bin"
Start-Sleep -Seconds 4
Send-Cmd "ls"
Wait-MatchAfter "vim" 90 "ls lists usr/bin (vim)" (Read-Log)
Start-Sleep -Seconds 2

Write-Host "=== 4) cat relative path (vim in cwd) ==="
Send-Cmd "cat vim"
Wait-MatchAfter "ELF" 60 "cat relative (vim -> ELF)" (Read-Log)
Start-Sleep -Seconds 2

Write-Host "=== 5) cd / ; pwd via /bin/pwd ==="
Send-Cmd "cd /"
Start-Sleep -Seconds 3
Send-Cmd "/bin/pwd"
Wait-MatchAfter "(?m)^/$" 30 "/bin/pwd = /" (Read-Log)
Start-Sleep -Seconds 2

$p.Kill()
$p.WaitForExit()
Unregister-Event -SourceIdentifier $handler.Name -ErrorAction SilentlyContinue
Write-Host "=== done ==="
