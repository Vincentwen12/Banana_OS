# W8 acceptance: PCI enumeration, RTL8139 driver, ARP cache and ICMP echo
# against QEMU user-mode networking (guest 10.0.2.15/24, gateway 10.0.2.2).
#
# Two phases, because the kernel shell and bash cannot both own the keyboard:
#   Phase 1 - boots disk.img + fs.img: asserts net_init() probed eth0 and that
#             the existing bash login path still comes up unchanged.
#   Phase 2 - boots disk.img only (no fs.img, so /bin/bash cannot load and the
#             kernel shell takes over): drives lspci / ifconfig / arp / ping.
#
# Usage:    powershell -NoProfile -ExecutionPolicy Bypass -File .\test_net.ps1
# Requires: disk.img / fs.img built by build.ps1, plus QEMU.
# QEMU is resolved from $env:QEMU, then .\qemu-system-x86_64.exe, then the
# default D:\qemu\ path; set $env:QEMU to use your own installation.
$ErrorActionPreference = "Continue"

$QemuExe = if ($env:QEMU) { $env:QEMU }
        elseif (Test-Path (Join-Path $PSScriptRoot "qemu-system-x86_64.exe")) { Join-Path $PSScriptRoot "qemu-system-x86_64.exe" }
        else { "D:\qemu\qemu-system-x86_64.exe" }
if (-not (Test-Path $QemuExe)) {
    Write-Host "[ERROR] QEMU not found: $QemuExe"
    Write-Host "        Set `$env:QEMU=<path to qemu-system-x86_64.exe> and retry."
    exit 2
}
$diskImg = Join-Path $PSScriptRoot "disk.img"
$fsImg   = Join-Path $PSScriptRoot "fs.img"
foreach ($img in @($diskImg, $fsImg)) {
    if (-not (Test-Path $img)) { Write-Host "[ERROR] missing $img - run .\build.ps1 first"; exit 2 }
}

$NetArgs = "-device rtl8139,netdev=n0 -netdev user,id=n0"

function Start-NetQemu([string]$logPath, [string]$drives) {
    Remove-Item $logPath -ErrorAction SilentlyContinue
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $QemuExe
    $psi.Arguments = "$drives -m 2G -nographic -no-shutdown $NetArgs -smp 1 -machine pc,accel=tcg"
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $handler = Register-ObjectEvent -InputObject $p -EventName OutputDataReceived -MessageData $logPath -Action {
        if ($EventArgs.Data) {
            [System.IO.File]::AppendAllText($Event.MessageData, $EventArgs.Data + "`n")
        }
    }
    $p.BeginOutputReadLine()
    return @{ proc = $p; handler = $handler; log = $logPath }
}

function Read-Log([hashtable]$ctx) {
    for ($i = 0; $i -lt 5; $i++) {
        try { return [System.IO.File]::ReadAllText($ctx.log) }
        catch { Start-Sleep -Milliseconds 200 }
    }
    return ""
}

function Wait-MatchAfter([hashtable]$ctx, [string]$pattern, [int]$secs, [string]$label, [string]$since) {
    $deadline = (Get-Date).AddSeconds($secs)
    while ((Get-Date) -lt $deadline) {
        $log = Read-Log $ctx
        $tail = ""
        if ($log.Length -gt $since.Length) { $tail = $log.Substring($since.Length) }
        if ($tail -match $pattern) { Write-Host "[OK] $label"; return $true }
        if ($tail -match "PANIC|panic:|triple fault|user fault") { Write-Host "[CRASH] $label"; return $false }
        Start-Sleep -Milliseconds 300
    }
    Write-Host "[TIMEOUT] $label (waiting for: $pattern)"
    return $false
}

# Feed in small chunks so QEMU's 16B 16550 FIFO never overflows.
function Send-Line([hashtable]$ctx, [string]$cmd) {
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($cmd + "`n")
    for ($i = 0; $i -lt $bytes.Length; $i += 8) {
        $hi = [Math]::Min($i + 7, $bytes.Length - 1)
        $chunk = [System.Text.Encoding]::ASCII.GetString($bytes[$i..$hi])
        $ctx.proc.StandardInput.Write($chunk)
        $ctx.proc.StandardInput.Flush()
        Start-Sleep -Milliseconds 25
    }
}

function Stop-NetQemu([hashtable]$ctx) {
    try { $ctx.proc.Kill(); $ctx.proc.WaitForExit() } catch { }
    Unregister-Event -SourceIdentifier $ctx.handler.Name -ErrorAction SilentlyContinue
}

$results = @()

# ---- Phase 1: net_init + bash login path ----
Write-Host "=== Phase 1: net_init + bash login path ==="
$ctx = Start-NetQemu (Join-Path $PSScriptRoot "net_phase1.log") "-drive format=raw,file=`"$diskImg`" -drive format=raw,file=`"$fsImg`""
$log = Read-Log $ctx
if (Wait-MatchAfter $ctx "\[NET\] eth0 mac=([0-9a-f]{2}:){5}[0-9a-f]{2} ip=10\.0\.2\.15/24 gw=10\.0\.2\.2" 60 "eth0 probed with 10.0.2.15/24" $log) { $results += "netinit:OK" } else { $results += "netinit:FAIL" }
$log = Read-Log $ctx
if (Wait-MatchAfter $ctx "Running /bin/bash" 90 "bash login path intact" $log) { $results += "bash:OK" } else { $results += "bash:FAIL" }
Stop-NetQemu $ctx

# ---- Phase 2: kernel shell network commands ----
Write-Host "=== Phase 2: lspci / ifconfig / arp / ping ==="
$ctx = Start-NetQemu (Join-Path $PSScriptRoot "net_phase2.log") "-drive format=raw,file=`"$diskImg`""
$log = Read-Log $ctx
if (Wait-MatchAfter $ctx "kernel shell" 60 "kernel shell ready" $log) { $results += "shell:OK" } else { $results += "shell:FAIL" }
Start-Sleep -Seconds 1

$log = Read-Log $ctx
Send-Line $ctx "lspci"
if (Wait-MatchAfter $ctx "10EC:8139" 15 "lspci lists RTL8139 (10EC:8139)" $log) { $results += "lspci:OK" } else { $results += "lspci:FAIL" }

$log = Read-Log $ctx
Send-Line $ctx "ifconfig"
# One snapshot serves all three assertions: the whole ifconfig block arrives at
# once, so a later snapshot would already contain the lines and match nothing.
if (Wait-MatchAfter $ctx "eth0: flags=UP" 15 "ifconfig shows eth0" $log) { $results += "ifconfig:OK" } else { $results += "ifconfig:FAIL" }
if (Wait-MatchAfter $ctx "ether ([0-9a-f]{2}:){5}[0-9a-f]{2}" 5 "ifconfig shows MAC" $log) { $results += "mac:OK" } else { $results += "mac:FAIL" }
if (Wait-MatchAfter $ctx "inet 10\.0\.2\.15/24 gw 10\.0\.2\.2" 5 "ifconfig shows address + gateway" $log) { $results += "addr:OK" } else { $results += "addr:FAIL" }

$log = Read-Log $ctx
Send-Line $ctx "ping 10.0.2.2"
if (Wait-MatchAfter $ctx "4/4 replies" 30 "ping gateway 4/4 replies" $log) { $results += "ping:OK" } else { $results += "ping:FAIL" }

$log = Read-Log $ctx
Send-Line $ctx "arp"
if (Wait-MatchAfter $ctx "(?m)^\s+10\.0\.2\.2\s+([0-9a-f]{2}:){5}[0-9a-f]{2}" 15 "arp cache learned gateway" $log) { $results += "arp:OK" } else { $results += "arp:FAIL" }

# ---- UDP end-to-end: a host-side echo listener proves a real round trip ----
# QEMU user-mode networking maps guest -> 10.0.2.2:<port> onto the host loopback,
# so a listener bound to 127.0.0.1:<port> receives the datagram and can echo it
# straight back into the guest. Binding loopback only also keeps Windows
# Firewall out of the picture.
try {
    $udpEp = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, 5555)
    $udpSock = New-Object System.Net.Sockets.UdpClient($udpEp)
    $udpSock.Client.ReceiveTimeout = 5000
} catch {
    Write-Host "[ERROR] cannot bind 127.0.0.1:5555 - $($_.Exception.Message)"
    Stop-NetQemu $ctx
    exit 2
}

$log = Read-Log $ctx
Send-Line $ctx "udp send 10.0.2.2 5555 8"

$remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
try {
    $echo = $udpSock.Receive([ref]$remote)
    [void]$udpSock.Send($echo, $echo.Length, $remote)
} catch {
    Write-Host "[TIMEOUT] host echo listener received nothing ($($_.Exception.Message))"
}
$udpSock.Close()

if (Wait-MatchAfter $ctx "reply 8 bytes from [0-9.]+:5555" 20 "udp round trip" $log) { $results += "udp:OK" } else { $results += "udp:FAIL" }
if (Wait-MatchAfter $ctx "data=BANANAUD" 5 "udp echoed payload intact" $log) { $results += "udpecho:OK" } else { $results += "udpecho:FAIL" }

$log = Read-Log $ctx
Send-Line $ctx "udp stat"
if (Wait-MatchAfter $ctx "UDP rx [1-9][0-9]* tx [1-9][0-9]*" 15 "udp counters advanced" $log) { $results += "udpstat:OK" } else { $results += "udpstat:FAIL" }

Stop-NetQemu $ctx

$fail = 0
foreach ($r in $results) { Write-Host "  $r"; if ($r -match "FAIL") { $fail++ } }
if ($fail -eq 0) { Write-Host "=== NET TEST PASSED ==="; exit 0 }
else { Write-Host "=== NET TEST FAILED ==="; exit 1 }
