# Banana_OS

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

A from-scratch **x86-64 operating system kernel** that boots from a hand-written
BIOS boot sector and runs a **real Ubuntu userland** on top of its own EXT2
driver and Linux-compatible syscall layer — GNU bash 5.2, coreutils, GCC 11,
GNU Make, Python 3.10 and Vim all execute as user processes.

> 中文版文档：[README.zh-CN.md](README.zh-CN.md)

---

## Highlights

| Subsystem | Design |
|---|---|
| **Boot** | 16-bit BIOS boot sector (`bootsect.S`, CHS) → `kernel.flat` → long-mode entry (`boot.S`). A Multiboot2 header is also embedded, so `qemu -kernel kernel.bin` works too. |
| **Ω Elastic memory** | Bitmap allocator over three physical zones — hot `[8MB, 1.03GB)`, warm, boomerang pool — plus a hot-page free cache, SWAR hotness tracking and XOR page compression. |
| **Ψ Fair scheduler** | 5-level MLFQ with priority aging and foreground preemption; cooperative switching (no timer preemption of user tasks). |
| **Δ Doorbell IPC** | 64 channels with per-task ACL masks; wake-up latency target < 1 µs. |
| **Σ Power manager** | C-state management via `MONITOR`/`MWAIT`, frequency governor and a policy engine; idle APs park in C-state and are woken through a dedicated doorbell channel. |
| **SMP** | INIT-SIPI AP bring-up, per-AP doorbell wake-up, per-CPU state reporting (`power status`). |
| **Virtual memory** | Per-task 4-level page tables; kernel identity map kept in the supervisor high range, user pages mapped with `U/S=1`. Lazy 2 MB/1 GB huge-page splitting down to 4 KB on demand. |
| **Filesystem** | VFS with devfs / tmpfs / procfs / sysfs / pipe, and a read-only EXT2 driver (4 KB blocks, superblock + group descriptors + inodes + directories + symlinks) over an ATA PIO driver (28-bit LBA, polled, no DMA). |
| **Userland** | ELF loader with `PT_INTERP` support (dynamic linking through `ld-linux`), `syscall`/`sysret` entry, POSIX signals with sigframe + restorer stub, `fork`/`clone`/`execve`/`wait4`, futex wait queues. |
| **Linux ABI** | 60+ Linux/x86-64 syscall handlers in `src/kernel/syscall/table.c`, enough to run glibc binaries from Ubuntu 22.04 (jammy). |

---

## Repository layout

```
.
├── src/
│   ├── boot/                 # bootsect.S, boot.S, AP trampoline
│   ├── include/              # axion.h, bananaos.h — shared types & constants
│   └── kernel/
│       ├── core/             # kmain, GDT/IDT, VGA, keyboard, timer, shell, printk
│       ├── mm/               # Ω allocator, vm, hotness, compress, boomerang
│       ├── sched/            # Ψ MLFQ scheduler
│       ├── ipc/              # Δ doorbell IPC
│       ├── power/            # Σ cstate / freq / policy
│       ├── syscall/          # syscall entry, dispatch table, signals, shm
│       ├── elf/              # ELF loader, interpreter, embedded hello.elf
│       ├── fs/               # VFS, devfs, tmpfs, procfs, sysfs, pipe, ext2
│       ├── dev/              # ATA PIO block device
│       └── net/              # loopback
├── tools/                    # rootfs fetch, EXT2 image builder, ELF generators
├── iso/grub/                 # GRUB config (legacy path, unused by build.ps1)
├── x86_64-elf/               # cross binutils   ← NOT in the repo, see below
├── .trae/                    # design specs and fix reports (docs, not build)
├── linker.ld                 # kernel load address 0x100000, PVH note, sections
├── build.ps1                 # ← authoritative build & run script
├── clean_asm.ps1             # strips GCC-generated directives for the ELF assembler
└── Makefile                  # early-stage build script (stale paths, see note)
```

### User/kernel address map

| Range | Use |
|---|---|
| `0x00100000` | kernel image (1 MB, Multiboot convention) |
| `0x00800000 – 0x40800000` | Ω hot zone |
| `0x40800000 – 0x60800000` | warm zone |
| `0x60800000 – 0x70800000` | boomerang pool |
| `0xC0000000 – 0x100000000` | LAPIC |
| `0x4000000000` | **user program base** (256 GB — above every kernel physical zone, so user mappings can never shadow the kernel's identity-mapped accesses) |
| `0x8000000000` | user stack top |
| `0x7F0000000000` | `ld-linux` / dynamic linker |
| `0x7F0000100000+` | `mmap` hint area |

---

## Requirements

- **Windows 10/11** with PowerShell (the build system is PowerShell-based)
- **MinGW-w64 GCC** available as `gcc` (used as a C→assembly frontend; the kernel is built `-ffreestanding`)
- **x86_64-elf binutils**: `as`, `ld`, `objcopy` — placed in `./x86_64-elf/bin/`
- **Python 3** (`fetch_deps.py`, `ext2_mkfs.py`, `gen_*.py`)
- **QEMU** — default path `D:\qemu\qemu-system-x86_64.exe`, edit `build.ps1` to change it

`./x86_64-elf/` and `./tools/rootfs/` are intentionally **not** committed
(they are ~100 MB and ~470 MB). Recreate them with the steps below.

---

## Quick start

```powershell
# 1. Toolchain: provide x86_64-elf binutils at ./x86_64-elf/bin/
#    (build binutils with --target=x86_64-elf, or drop in an existing
#     x86_64-elf toolchain shipped for Windows)

# 2. Fetch the Ubuntu jammy userland (~470 MB unpacked)
#    Downloads .deb packages into tools/debs/, unpacks into tools/rootfs/,
#    and regenerates tools/fs_manifest.txt (the EXT2 injection list).
python tools/fetch_deps.py

# 3. Build the kernel and the disk images
.\build.ps1 -SkipFs      # kernel only (disk.img)
.\build.ps1              # kernel + rebuild fs.img from tools/rootfs

# 4. Boot it
.\build.ps1 run          # QEMU, serial console
.\build.ps1 run-gui      # QEMU, graphical window
.\build.ps1 debug        # QEMU waiting for GDB on :1234
.\build.ps1 clean        # remove build outputs
```

Under the hood step 4 runs:

```
qemu-system-x86_64 -drive format=raw,file=disk.img \
                   -drive format=raw,file=fs.img \
                   -m 2G -nographic -no-shutdown \
                   -smp 4,sockets=1,cores=4,threads=1 -machine pc,accel=tcg
```

`fs.img` is the EXT2 root disk (512 MB, 4096-byte blocks) built by
`tools/ext2_mkfs.py` from the manifest; `disk.img` is the boot disk holding the
boot sector plus `kernel.flat`.

### Build artifacts

| File | Meaning | Typical size |
|---|---|---|
| `kernel.bin` | linked ELF | ~171 KB |
| `kernel.flat` | flat binary loaded by the boot sector | ~137 KB |
| `bootsect.bin` | 512-byte boot sector | 512 B |
| `disk.img` | boot disk (sector 0 + kernel) | ~516 KB |
| `fs.img` | EXT2 root filesystem | 512 MB |

The boot sector reads at most `KERNEL_SECTORS = 1007` sectors (~503 KB) of
kernel image, so `kernel.flat` growth is bounded by the boot loader.

---

## Shell commands

The built-in kernel shell (`src/kernel/core/shell.c`) provides:

`help` `hello` `mem` `alloc` `free` `clear` `reboot` `panic` `echo` `stats`
`compress` `ps` `kill` `ipc` `power` `run` `jobs` `fg` `bg` `ls` `cat` `dmesg`
`fsalloc` `fsfree` `wfile`

After booting, the kernel hands over to `/bin/bash` from the EXT2 disk, giving a
full interactive shell with job control.

---

## Tests

The QEMU-driven regression scripts (`test_*.ps1`, `iso_*.ps1`, plus the image
checkers `chk_fsimg*.py` / `verify_fs_state.py`) live in the local development
tree and are **not** tracked by git — see `.gitignore`. They boot
`disk.img` / `fs.img` under QEMU, drive the serial console and assert on the
output, covering: fork/exec/ELF regression (10 consecutive
`python3 -c "import ..."` runs), bash restart / login loop, toolchain
availability (vim, gcc, make, tar, python3 REPL), `ls`/`cd` relative paths,
pipes, and EXT2 image contents.

```powershell
# in the local development tree
.\build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\iso_s_imports.ps1
```

---

## Design documents

`.trae/documents/` and `.trae/specs/` hold the working specs and post-mortems
for each milestone (W1 lightning boot → W7 userland toolchain), including the
root-cause reports behind the trickier bugs (identity-map shadowing, `wait4`
stack restoration, fd-table collisions, brk/mmap heap corruption). They are kept
in the repository as the project's engineering log.

---

## Known limitations

- **No threads**: `clone()` with a non-NULL stack returns `ENOSYS`; `fork()` is a
  full deep copy of the address space (no copy-on-write).
- **Cooperative scheduling**: user tasks yield voluntarily; there is no timer
  preemption of ring-3 code.
- **`mprotect` is a stub** — it returns success without changing page
  permissions; a few other syscalls are accepted no-ops for glibc's benefit.
- **EXT2 is read-only** (write-side helpers exist for the kernel shell only).
- **`Makefile` is stale**: it predates the module split (`src/kernel/mm/`,
  `sched/`, ...) and points at old paths. Use `build.ps1`.
- Test scripts hard-code the QEMU path `D:\qemu\`.

---

## License

Released under the [MIT License](LICENSE) — © 2026 Vincentwen12.

The Ubuntu userland shipped into `fs.img` is **not** covered by this license;
see the acknowledgements below.

## Acknowledgements

The userland shipped into `fs.img` is unpacked from Ubuntu 22.04 (jammy) `.deb`
packages by `tools/fetch_deps.py`; those binaries are Ubuntu's, not this
project's, and remain under their respective licenses. Only the kernel, boot
loader, drivers, build scripts and tooling in `src/` and `tools/` are original
work.
