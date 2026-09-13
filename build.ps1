# Axion-Ban Kernel Build Script (PowerShell)
# 用法: .\build.ps1 [all|run|run-gui|debug|clean]

param([string]$Target = "all", [string]$FsSize = "512M", [switch]$SkipFs)

$ErrorActionPreference = "Stop"

# 工具路径
$AS      = "$PSScriptRoot\x86_64-elf\bin\as.exe"
$LD      = "$PSScriptRoot\x86_64-elf\bin\ld.exe"
$OBJCOPY = "$PSScriptRoot\x86_64-elf\bin\objcopy.exe"
$CC      = "gcc"
$QEMU    = "D:\qemu\qemu-system-x86_64.exe"

function Invoke-GCC {
    param([string]$Out, [string]$In)
    & $script:CC -S -m64 -mno-red-zone -nostdlib -ffreestanding `
         -fno-builtin -fno-pic -fno-asynchronous-unwind-tables `
         -fno-exceptions -fno-stack-check -fno-stack-protector `
         -Wall -O2 -mcmodel=large -mabi=sysv `
         -Isrc/include -Isrc/kernel -Isrc/kernel/core -Isrc/kernel/sched `
         -Isrc/kernel/ipc -Isrc/kernel/ap -Isrc/kernel/mm -Isrc/kernel/power `
         -Isrc/kernel/syscall -Isrc/kernel/elf -Isrc/kernel/fs -Isrc/kernel/dev `
         -Isrc/kernel/net `
         -o $Out $In
}

function Stop-Qemu {
    $qemu = Get-Process -Name "qemu-system-x86_64" -ErrorAction SilentlyContinue
    if ($qemu) {
        Write-Host "[QEMU] Killing existing QEMU process..." -ForegroundColor Yellow
        $qemu | Stop-Process -Force
        Start-Sleep -Milliseconds 500
    }
}

function Build-Kernel {
    Write-Host "=== Axion-Ban Kernel Build ===" -ForegroundColor Cyan

    $bootS   = "src/boot/boot.S"
    $cSrcs = @(
        "src/kernel/core/vga.c", "src/kernel/core/keyboard.c", "src/kernel/core/timer.c",
        "src/kernel/core/gdt.c", "src/kernel/core/idt.c",
        "src/kernel/mm/mm.c", "src/kernel/mm/vm.c", "src/kernel/mm/hotness.c", "src/kernel/mm/compress.c",
        "src/kernel/mm/boomerang.c", "src/kernel/ap/ap.c", "src/kernel/ipc/ipc.c",
        "src/kernel/sched/sched.c", "src/kernel/ipc/doorbell.c", "src/kernel/core/shell.c",
        "src/kernel/core/panic.c", "src/kernel/core/string.c", "src/kernel/core/printk.c",
        "src/kernel/kmain.c",
        "src/kernel/power/freq.c", "src/kernel/power/cstate.c", "src/kernel/power/policy.c",
        "src/kernel/syscall/syscall.c", "src/kernel/syscall/table.c",
        "src/kernel/syscall/signal.c", "src/kernel/syscall/shm.c",
        "src/kernel/net/net.c", "src/kernel/net/loopback.c",
        "src/kernel/net/rtl8139.c", "src/kernel/net/eth.c",
        "src/kernel/net/ip.c", "src/kernel/net/udp.c",
        "src/kernel/elf/loader.c", "src/kernel/elf/interp.c", "src/kernel/elf/hello_init.c",
        "src/kernel/fs/vfs.c", "src/kernel/fs/devfs.c", "src/kernel/fs/tmpfs.c",
        "src/kernel/fs/procfs.c", "src/kernel/fs/sysfs.c", "src/kernel/fs/pipe.c",
        "src/kernel/fs/ext2.c",
        "src/kernel/dev/ata.c", "src/kernel/dev/pci.c"
    )

    # 汇编 boot.S (64-bit)
    Write-Host "[AS] boot.S" -ForegroundColor Yellow
    $bootObj = [IO.Path]::ChangeExtension($bootS, ".o")
    & $script:AS --64 -o $bootObj $bootS
    if ($LASTEXITCODE -ne 0) { throw "Assembly failed: $bootS" }

    # 汇编 AP trampoline (16/32/64-bit mixed, 编译为 flat binary 后嵌入)
    Write-Host "[AS] ap_trampoline.S" -ForegroundColor Yellow
    $trampS = "src/boot/ap_trampoline.S"
    $trampObj = "src/boot/ap_trampoline.o"
    $trampBin = "ap_trampoline.bin"
    $trampEmbed = "ap_trampoline_embed.o"
    & $script:AS --32 -o $trampObj $trampS
    if ($LASTEXITCODE -ne 0) { throw "Assembly failed: $trampS" }
    & $script:OBJCOPY -O binary -j .text $trampObj $trampBin
    if ($LASTEXITCODE -ne 0) { throw "objcopy failed: $trampObj" }
    # 嵌入为 ELF 对象 (符号: _binary_ap_trampoline_bin_start, _binary_ap_trampoline_bin_end, _binary_ap_trampoline_bin_size)
    & $script:OBJCOPY -I binary -O elf64-x86-64 -B i386:x86-64 $trampBin $trampEmbed
    if ($LASTEXITCODE -ne 0) { throw "objcopy embed failed: $trampBin" }
    Remove-Item $trampObj -Force -ErrorAction SilentlyContinue

    # 汇编 syscall_entry.S (64-bit)
    $syscallEntryS = "src/kernel/syscall/syscall_entry.S"
    $syscallEntryObj = [IO.Path]::ChangeExtension($syscallEntryS, ".o")
    Write-Host "[AS] syscall_entry.S" -ForegroundColor Yellow
    & $script:AS --64 -o $syscallEntryObj $syscallEntryS
    if ($LASTEXITCODE -ne 0) { throw "Assembly failed: $syscallEntryS" }

    # 汇编 idt.S (64-bit) — 异常入口 thunk (对象名区别于 idt.c 的 idt.o)
    $idtS = "src/kernel/core/idt.S"
    $idtObj = "src/kernel/core/idt_asm.o"
    Write-Host "[AS] idt.S" -ForegroundColor Yellow
    & $script:AS --64 -o $idtObj $idtS
    if ($LASTEXITCODE -ne 0) { throw "Assembly failed: $idtS" }

    $allObjs = @($bootObj, $trampEmbed, $syscallEntryObj, $idtObj)

    # 编译所有 C 文件
    foreach ($c in $cSrcs) {
        $tmp = [IO.Path]::ChangeExtension($c, ".o.tmp.s")
        $obj = [IO.Path]::ChangeExtension($c, ".o")
        $name = [IO.Path]::GetFileNameWithoutExtension($c)

        Write-Host "[CC] $name" -ForegroundColor Yellow
        Invoke-GCC -Out $tmp -In $c
        if ($LASTEXITCODE -ne 0) { throw "GCC failed: $c" }

        # 清理汇编
        & powershell -File "$PSScriptRoot\clean_asm.ps1" $tmp

        # 汇编
        & $script:AS --64 -o $obj $tmp
        if ($LASTEXITCODE -ne 0) { throw "Assembly failed: $c" }
        Remove-Item $tmp -Force -ErrorAction SilentlyContinue

        $allObjs += $obj
    }

    # 链接 ELF
    Write-Host "[LD] kernel.bin" -ForegroundColor Yellow
    & $script:LD -T linker.ld -nostdlib -o kernel.bin $allObjs
    if ($LASTEXITCODE -ne 0) { throw "Link failed" }

    $size = (Get-Item kernel.bin).Length
    Write-Host "[OK] kernel.bin ($size bytes)" -ForegroundColor Green

    # 提取 flat binary
    Write-Host "[OBJCOPY] kernel.flat" -ForegroundColor Yellow
    & $script:OBJCOPY -O binary kernel.bin kernel.flat
    if ($LASTEXITCODE -ne 0) { throw "objcopy failed" }
    $flatSize = (Get-Item kernel.flat).Length
    Write-Host "[OK] kernel.flat ($flatSize bytes)" -ForegroundColor Green

    # 汇编 bootsect.S (16-bit)
    Write-Host "[AS] bootsect.S" -ForegroundColor Yellow
    & $script:AS --32 -o src/boot/bootsect.o src/boot/bootsect.S
    if ($LASTEXITCODE -ne 0) { throw "Assembly failed: bootsect.S" }
    & $script:OBJCOPY -O binary -j .text src/boot/bootsect.o bootsect.bin
    if ($LASTEXITCODE -ne 0) { throw "objcopy failed: bootsect.o" }
    Remove-Item src/boot/bootsect.o -Force -ErrorAction SilentlyContinue

    # 构建磁盘镜像
    Write-Host "[DISK] disk.img" -ForegroundColor Yellow
    Stop-Qemu
    $minKernelSectors = 1007  # must match KERNEL_SECTORS in bootsect.S (CHS max ~503KB)
    $neededSectors = [math]::Ceiling($flatSize / 512)
    if ($neededSectors -gt $minKernelSectors) {
        throw "kernel.flat ($flatSize bytes) exceeds boot sector capacity (KERNEL_SECTORS=$minKernelSectors sectors)"
    }
    $kernelSectors = [math]::Max($neededSectors, $minKernelSectors)
    $diskSize = (1 + $kernelSectors) * 512
    $disk = New-Object byte[] $diskSize

    # 写入 bootsect (扇区 0)
    $bootBin = [System.IO.File]::ReadAllBytes("$PSScriptRoot\bootsect.bin")
    [Array]::Copy($bootBin, 0, $disk, 0, [math]::Min($bootBin.Length, 510))
    $disk[510] = 0x55
    $disk[511] = 0xAA

    # 写入内核 (扇区 1+)
    $flatBin = [System.IO.File]::ReadAllBytes("$PSScriptRoot\kernel.flat")
    [Array]::Copy($flatBin, 0, $disk, 512, $flatBin.Length)

    [System.IO.File]::WriteAllBytes("$PSScriptRoot\disk.img", $disk)
    Write-Host "[OK] disk.img (boot + kernel, $diskSize bytes)" -ForegroundColor Green

    # 编译用户态工具 tools/sleep.S -> tools/sleep（经 fs_manifest.txt 注入 /bin/sleep）
    Write-Host "[AS] tools/sleep.S" -ForegroundColor Yellow
    & $script:AS --64 -o "$PSScriptRoot\tools\sleep.o" "$PSScriptRoot\tools\sleep.S"
    if ($LASTEXITCODE -ne 0) { throw "Assembly failed: tools/sleep.S" }
    Write-Host "[LD] tools/sleep" -ForegroundColor Yellow
    & $script:LD -e _start --oformat elf64-x86-64 -Ttext 0x400000 `
        -o "$PSScriptRoot\tools\sleep" "$PSScriptRoot\tools\sleep.o"
    if ($LASTEXITCODE -ne 0) { throw "Link failed: tools/sleep" }
    Remove-Item "$PSScriptRoot\tools\sleep.o" -Force -ErrorAction SilentlyContinue

    # 生成 EXT2 测试盘 (第二个 IDE 盘)，默认 512MB/4096B/4 组（W7 Phase 3.2）
    if (-not $SkipFs) {
        Write-Host "[EXT2] fs.img ($FsSize)" -ForegroundColor Yellow
        & python "$PSScriptRoot\tools\ext2_mkfs.py" `
            --size $FsSize --block-size 4096 --blocks-per-group 32768 `
            --inodes-per-group 8192 "$PSScriptRoot\fs.img"
        if ($LASTEXITCODE -ne 0) { throw "ext2_mkfs.py failed" }
        $fsSizeBytes = (Get-Item "$PSScriptRoot\fs.img").Length
        Write-Host "[OK] fs.img ($fsSizeBytes bytes)" -ForegroundColor Green
    } else {
        Write-Host "[SKIP] fs.img (keep existing)" -ForegroundColor DarkGray
    }
}

function Invoke-Clean {
    Write-Host "[CLEAN]" -ForegroundColor Yellow
    Remove-Item src/boot/*.o -Force -ErrorAction SilentlyContinue
    Remove-Item src/kernel/*.o -Force -ErrorAction SilentlyContinue
    Remove-Item src/kernel/*.tmp.s -Force -ErrorAction SilentlyContinue
    Remove-Item src/kernel/core/*.o -Force -ErrorAction SilentlyContinue
    Remove-Item src/kernel/core/*.tmp.s -Force -ErrorAction SilentlyContinue
    Remove-Item src/kernel/sched/*.o -Force -ErrorAction SilentlyContinue
    Remove-Item src/kernel/sched/*.tmp.s -Force -ErrorAction SilentlyContinue
    Remove-Item src/kernel/ipc/*.o -Force -ErrorAction SilentlyContinue
    Remove-Item src/kernel/ipc/*.tmp.s -Force -ErrorAction SilentlyContinue
    Remove-Item src/kernel/ap/*.o -Force -ErrorAction SilentlyContinue
    Remove-Item src/kernel/ap/*.tmp.s -Force -ErrorAction SilentlyContinue
    Remove-Item src/kernel/mm/*.o -Force -ErrorAction SilentlyContinue
    Remove-Item src/kernel/mm/*.tmp.s -Force -ErrorAction SilentlyContinue
    Remove-Item src/kernel/power/*.o -Force -ErrorAction SilentlyContinue
    Remove-Item src/kernel/power/*.tmp.s -Force -ErrorAction SilentlyContinue
    Remove-Item src/kernel/syscall/*.o -Force -ErrorAction SilentlyContinue
    Remove-Item src/kernel/syscall/*.tmp.s -Force -ErrorAction SilentlyContinue
    Remove-Item src/kernel/elf/*.o -Force -ErrorAction SilentlyContinue
    Remove-Item src/kernel/elf/*.tmp.s -Force -ErrorAction SilentlyContinue
    Remove-Item src/kernel/fs/*.o -Force -ErrorAction SilentlyContinue
    Remove-Item src/kernel/fs/*.tmp.s -Force -ErrorAction SilentlyContinue
    Remove-Item src/kernel/dev/*.o -Force -ErrorAction SilentlyContinue
    Remove-Item src/kernel/dev/*.tmp.s -Force -ErrorAction SilentlyContinue
    Remove-Item src/kernel/net/*.o -Force -ErrorAction SilentlyContinue
    Remove-Item src/kernel/net/*.tmp.s -Force -ErrorAction SilentlyContinue
    Remove-Item kernel.bin, kernel.flat, bootsect.bin, disk.img -Force -ErrorAction SilentlyContinue
    Remove-Item ap_trampoline.bin, ap_trampoline_embed.o -Force -ErrorAction SilentlyContinue
    Write-Host "[OK] Clean done" -ForegroundColor Green
}

# 主入口
Push-Location $PSScriptRoot

switch ($Target) {
    "clean" {
        Invoke-Clean
        break
    }
    "run" {
        Build-Kernel
        Write-Host "[QEMU] Running (nographic)..." -ForegroundColor Cyan
        & $QEMU -drive format=raw,file=disk.img -drive format=raw,file=fs.img -m 2G -nographic -no-shutdown -device rtl8139,netdev=n0 -netdev user,id=n0 -smp 4,sockets=1,cores=4,threads=1 -machine pc,accel=tcg
        break
    }
    "run-gui" {
        Build-Kernel
        Write-Host "[QEMU] Running (GUI)..." -ForegroundColor Cyan
        & $QEMU -drive format=raw,file=disk.img -drive format=raw,file=fs.img -m 2G -device rtl8139,netdev=n0 -netdev user,id=n0 -smp 2,sockets=1,cores=2,threads=1 -machine pc,accel=tcg
        break
    }
    "debug" {
        Build-Kernel
        Write-Host "[QEMU] Debug mode (waiting on :1234)..." -ForegroundColor Cyan
        & $QEMU -drive format=raw,file=disk.img -drive format=raw,file=fs.img -m 2G -nographic -no-reboot -device rtl8139,netdev=n0 -netdev user,id=n0 -s -S -smp 2,sockets=1,cores=2,threads=1 -machine pc,accel=tcg
        break
    }
    default {
        Build-Kernel
        break
    }
}

Pop-Location