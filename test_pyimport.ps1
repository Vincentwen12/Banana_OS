# Phase B1 bisect: run python3 -c probes sequentially, capture OK marker or crash.
$ErrorActionPreference = "Continue"
$stream = Join-Path $PSScriptRoot "pyimport_stream.log"
Remove-Item $stream -ErrorAction SilentlyContinue

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = "D:\qemu\qemu-system-x86_64.exe"
$psi.Arguments = "-drive format=raw,file=disk.img -drive format=raw,file=fs.img -m 2G -nographic -no-shutdown -smp 1 -machine pc,accel=tcg"
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true

$p = [System.Diagnostics.Process]::Start($psi)
$handler = Register-ObjectEvent -InputObject $p -EventName OutputDataReceived -Action {
    if ($EventArgs.Data) {
        [System.IO.File]::AppendAllText("D:\BananaOS-Axion\pyimport_stream.log", $EventArgs.Data + "`n")
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
    Write-Host "[FAIL] $label"
    return $false
}
function Send-Line($cmd) {
    $p.StandardInput.WriteLine($cmd)
    $p.StandardInput.Flush()
}

$bootLog = Read-Log
if (-not (Wait-MatchAfter "Running /bin/bash" 90 "boot into bash" $bootLog)) { $p.Kill(); exit 1 }
Start-Sleep -Seconds 5
Send-Line "n"
Start-Sleep -Seconds 2
Send-Line ""
Start-Sleep -Seconds 6

$tests = @(
    @{ n = "print";      c = 'print(11+31)';                                          m = '42' },
    @{ n = "mod-ssl";    c = 'import ssl;print("OK-ssl")';                             m = 'OK-ssl' },
    @{ n = "mod-hash";   c = 'import hashlib;print(hashlib.md5(b"a").hexdigest())';    m = '0cc175b9c0f1b6a831c399e269772661' },
    @{ n = "mod-sqlite"; c = 'import sqlite3;print("OK-sqlite")';                      m = 'OK-sqlite' },
    @{ n = "mod-ctypes"; c = 'import ctypes;print("OK-ctypes")';                       m = 'OK-ctypes' },
    @{ n = "mod-decimal";c = 'import decimal;print(decimal.Decimal("1.5")+decimal.Decimal("2.5"))'; m = '4.0' },
    @{ n = "mod-bz2";    c = 'import bz2;print("OK-bz2")';                             m = 'OK-bz2' },
    @{ n = "mod-lzma";   c = 'import lzma;print("OK-lzma")';                           m = 'OK-lzma' },
    @{ n = "mod-crypt";  c = 'import crypt;print("OK-crypt")';                         m = 'OK-crypt' },
    @{ n = "mod-curses"; c = 'import curses;print("OK-curses")';                       m = 'OK-curses' },
    @{ n = "mod-readline";c = 'import readline;print("OK-readline")';                  m = 'OK-readline' },
    @{ n = "mod-uuid";   c = 'import uuid;print("OK-uuid")';                           m = 'OK-uuid' },
    @{ n = "mod-zoneinfo";c = 'import zoneinfo;print("OK-zoneinfo")';                  m = 'OK-zoneinfo' },
    @{ n = "mod-asyncio";c = 'import asyncio;print("OK-asyncio")';                     m = 'OK-asyncio' },
    @{ n = "mod-mmap";   c = 'import mmap;print("OK-mmap")';                           m = 'OK-mmap' },
    @{ n = "mod-termios";c = 'import termios;print("OK-termios")';                     m = 'OK-termios' },
    @{ n = "mod-array";  c = 'import array;print(array.array("i",[1,2,3])[1])';        m = '2' },
    @{ n = "mod-zlib";   c = 'import zlib;print(zlib.crc32(b"abc"))';                  m = '891568578' },
    @{ n = "mod-csv-json";c = 'import json,csv,re,subprocess,sysconfig;print("OK-std")'; m = 'OK-std' },
    @{ n = "file-run";   c = 'print(open("/root/py_diag.py","rb").read(40))';          m = 'b\'# Diagnose' }
)

foreach ($t in $tests) {
    $snap = Read-Log
    Send-Line ('python3 -c "' + $t.c + '"')
    Wait-MatchAfter $t.m 40 ("probe " + $t.n) $snap | Out-Null
    Start-Sleep -Milliseconds 800
}

$p.Kill(); $p.WaitForExit()
Unregister-Event -SourceIdentifier $handler.Name -ErrorAction SilentlyContinue
