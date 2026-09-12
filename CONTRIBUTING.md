# Contributing to Banana_OS

Thanks for your interest! This is a from-scratch x86-64 kernel, so a change is
most useful when it comes with a way to reproduce the behaviour under QEMU.

Issue 和 PR 用中文写也完全没问题。

## Before you start

Read [README.md](README.md) for the architecture overview and the address map,
then [docs/engineering-log/](docs/engineering-log/) for the post-mortems of the
trickier bugs. They document the non-obvious decisions — identity-map
shadowing, cooperative scheduling, the fd-table layout, brk/mmap heap growth —
and will save you from re-deriving (or re-breaking) them.

## Setting up

1. Windows 10/11 with PowerShell
2. MinGW-w64 `gcc` on `PATH` (used as a C→assembly frontend)
3. x86_64-elf binutils in `./x86_64-elf/bin/` — `as`, `ld`, `objcopy`
4. Python 3
5. QEMU — put `qemu-system-x86_64.exe` on `PATH` (or next to the scripts), or
   set `$env:QEMU` to its full path

```powershell
python tools/fetch_deps.py     # fetch the Ubuntu userland into tools/rootfs/
.\build.ps1                    # kernel + fs.img + disk.img
.\build.ps1 run                # boot it
```

## Before opening a PR

- `.\build.ps1 -SkipFs` succeeds without new warnings.
- The regression scripts pass — paste their output in the PR:

  ```powershell
  powershell -NoProfile -ExecutionPolicy Bypass -File .\iso_s_imports.ps1      # expect 10/10 ok
  powershell -NoProfile -ExecutionPolicy Bypass -File .\test_bash_restart.ps1  # expect PASSED
  ```

- `kernel.flat` stays inside the boot loader limit (1007 sectors ≈ 503 KB).
  `build.ps1` fails the build if it grows past that, so watch the reported size.

## Code conventions

- **C**: 4-space indent, kernel types from `src/include/axion.h`, no libc. The
  kernel is built `-ffreestanding`; only the `printk` formats documented in
  `src/kernel/core/printk.h` are available (`%d %u %x %p %s` — cast 64-bit
  values explicitly).
- **Assembly**: AT&T syntax, `.S` files are preprocessed.
- **Comments**: Chinese or English both fine — match the file you are editing.
- **New source files** belong in the matching `src/kernel/<module>/` directory
  and must be added to the `$cSrcs` list in `build.ps1`; nothing is auto-globbed.
- **Test scripts** stay in the local development tree. Only
  `iso_s_imports.ps1` and `test_bash_restart.ps1` are tracked; see `.gitignore`.

## Commit messages

A short imperative subject, then a body explaining *why* — the *what* is already
in the diff. Example:

```
Fix user mappings shadowing the kernel identity map

Physical allocation crossed 512 MB, so the identity linear slot of a freshly
allocated page was already taken by a user mapping; vm_zero then wrote into
bash's text page. Move the user program base above every kernel physical zone.
```

## Reporting bugs

Please include:

- the exact QEMU command line,
- the serial log up to the failure,
- expected vs. actual behaviour.

For user-mode crashes, `qemu-system-x86_64 ... -d int,cpu_reset -D log` is the
fastest way in: in that log `v=0d` is a user-mode `#GP`, `v=08` a `#DF`, and a
CPU reset right after it means a triple fault. The RIP/RSP snapshot usually
points straight at the faulting instruction.
