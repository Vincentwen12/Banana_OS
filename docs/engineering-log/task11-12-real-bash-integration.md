# Task 11 & 12：集成确认 + 真实动态链接 bash 端到端验收

## Summary

把当前"自举验证"阶段内核推进到"能跑真实程序"：Task 11 完成全局构建/初始化整合确认（clean build 无残留、子系统初始化顺序正确），Task 12 注入真实 glibc 动态链接 bash 5.2（含 ld-linux 与 libtinfo），使 `run /bin/bash` 启动到 `bash-5.2$` 交互提示符，支持内置命令与单条外部命令（**最小验收**，fork 同步模型不改）。

关键决策（用户已确认）：二进制从网络下载（不用 git，用 PowerShell `Invoke-WebRequest`/`curl.exe`）；验收范围 = 最小档；构建入口用 `.\build.ps1`（Makefile 已过时且工具链无 make.exe）。

---

## Current State Analysis（基于 Phase 1 探索）

| 维度 | 现状 | 问题 |
|---|---|---|
| 构建 | `build.ps1`（28 个 C 文件 + boot.S/ap_trampoline.S/syscall_entry.S/bootsect.S，链接到 0x100000，kernel.flat < 80KB，fs.img 由 ext2_mkfs.py 生成） | Makefile 过时（旧扁平路径），"make clean && make" 不可用 |
| 初始化 | kmain.c 顺序完整：vga→timer→mm→sched→doorbell→ata→ext2→kbd→vfs→devfs→tmpfs→gdt→syscall→ap→shell→自动测试 | 顺序正确，无需改动 |
| ELF/动态链接 | PT_INTERP 提取✓；elf_load_interp 以 0x7F0000000000 基址映射 ld-linux✓；auxv 含 AT_BASE/AT_PHDR/AT_ENTRY✓（AT_SYSINFO_EHDR=0，glibc 可回退 syscall 指令） | 无阻塞 |
| syscall | 23 个（read/write/open/close/mmap/munmap/brk/rt_sig*/fork/execve/exit/wait4/kill/uname/fcntl/getcwd/chdir/gettimeofday/exit_group） | **缺 glibc/bash 启动必需**：mprotect(10)、ioctl(16)、stat/fstat/lstat(4/5/6)、access(21)、dup2(33)、getuid 族(102/104/107/108)、getppid(110)、arch_prctl(158)、futex(202)、set_tid_address(218)、openat(257)、getrandom(318) |
| 文件打开 | `sys_open`（table.c）只走 devfs→tmpfs，**不走 ext2**；`elf_load` 走 `vfs_open`（devfs→ext2，不含 tmpfs） | 用户态无法 open 磁盘文件；两处不一致 |
| tmpfs /bin/bash | `hello_elf_init` 把 /bin/bash 注册为 hello.elf 伪 ELF | **会拦截真实 bash**，必须移除 |
| 终端 | /dev/tty：tty_read 逐字符 kbd_getchar（阻塞），tty_write→vga_putc；kbd_poll 同时轮询串口+PS2 | bash 自身做行编辑，逐字符 raw 可接受；isatty 需 ioctl(TCGETS) 成功 |
| 进程模型 | fork=同步子进程（子进程在父 wait4 内运行） | 最小验收（单条命令）够用 |

---

## Proposed Changes

### Task 11：集成初始化与构建确认

**T11.1 构建命令统一（决策）**
- 不修复 Makefile。执行阶段用 `.\build.ps1 clean` + `.\build.ps1 all` 验证无残留依赖（clean 删 .o/kernel.bin/disk.img 等，不删 fs.img）。
- 确认 cSrcs 已覆盖全部新文件（探索确认已含 loader/interp/syscall/table 等，无需改）。

**T11.2 初始化顺序确认**
- kmain.c 顺序已正确，无改动。仅在 Task 12 完成后更新 kmain 自动测试命令（见 T12.7）。

### Task 12：真实 bash 端到端（最小验收）

#### T12.1 下载并解包真实 bash 5.2（glibc 动态，Ubuntu 24.04 源）
- 文件：`tools/bash`、`tools/ld-linux-x86-64.so.2`、`tools/libtinfo.so.6`
- 步骤（不用 git）：
  1. `Invoke-WebRequest` 下载 `archive.ubuntu.com/ubuntu/pool/main/b/bash/` 的 `bash_5.2.21-*_amd64.deb`、`pool/main/g/glibc/` 的 `libc6_*.deb`、`pool/main/n/ncurses/` 的 `libtinfo6_*.deb`（版本以实际最新为准；失败则换 `deb.debian.org` Debian 12 bookworm 同名包）。
  2. 用 python 脚本解 .deb（ar 容器 → 提取 `data.tar.xz` → `tarfile` 解压）：
     - bash.deb → `/bin/bash` → `tools/bash`
     - libc6.deb → `/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2` → `tools/ld-linux-x86-64.so.2`
     - libtinfo6.deb → `/lib/x86_64-linux-gnu/libtinfo.so.6` → `tools/libtinfo.so.6`
  3. 用 `readelf -d tools/bash` 确认 PT_INTERP 路径与依赖（预期 `/lib64/ld-linux-x86-64.so.2` + libtinfo.so.6 + libc.so.6）。
- **回退**：若 glibc 启动被 arch_prctl/futex 等阻塞且调试成本过高，回退到 Alpine musl 动态 bash（APK 为 tar.gz 直接可解，musl 启动 syscall 需求显著更少）；再不行则改静态 busybox sh 作最终兜底（需向用户说明偏离）。

#### T12.2 文件系统镜像注入
- 文件：`tools/ext2_mkfs.py`（改 build()）、`tools/fs_manifest.txt`
- 做法：fs_manifest.txt 追加（host 文件相对 tools/ 路径）：
  ```
  bash:/bin/bash
  ld-linux-x86-64.so.2:/lib64/ld-linux-x86-64.so.2
  libtinfo.so.6:/lib/x86_64-linux-gnu/libtinfo.so.6
  ```
  `walk_dir` 自动创建 `/lib64`、`/lib/x86_64-linux-gnu`。若 bash 的 PT_INTERP 不是 `/lib64/...`，则按 readelf 实际值注入对应路径，并用现有 `create_symlink` 补一个 `/lib64/ld-linux-x86-64.so.2` 符号链接指向实际路径。

#### T12.2b ext2 两级间接块支持（**必须项**，bash 1.3MB / ld-linux 2MB 远超 12KB 直接块与 256KB 单级间接块上限）
- 文件：`tools/ext2_mkfs.py`、`src/kernel/fs/ext2.c`
- 生成器（ext2_mkfs.py `make_file`）：按文件大小分配块号并写三类指针：
  - 前 12 块 → `i_block[0..11]`
  - 后续块 → 单级间接块 `i_block[12]`（一个 1024B 块存 256 个块号）
  - 超过 12+256 块 → 两级间接块 `i_block[13]`（一个块存 256 个单级间接块号）
- 内核（ext2.c `ext2_data_block`）：在现有单级逻辑后补两级遍历（idx 先落在两级间接块 → 读单级间接块号 → 读数据块号），保持只读。
- 验证：`fs.img` 生成后 `readelf`/`xxd` 抽查 inode 块指针；内核 `run /bin/bash` 能读全文件（此前 test_fork 5128B 走直接块，本次是首个超 12KB 文件）。

#### T12.3 修复文件打开一致性（sys_open 走 ext2）
- 文件：`src/kernel/fs/vfs.c`、`src/kernel/fs/tmpfs.h`、`src/kernel/syscall/table.c`
- 做法：
  1. `vfs_open`（vfs.c）查找顺序改为 devfs → tmpfs → ext2（补 tmpfs 分支；tmpfs 有 `tmpfs_open` 接口）。
  2. `sys_open`（table.c）从"devfs→tmpfs"改为直接调用 `vfs_open`（统一路径，devfs/tmpfs/ext2 都覆盖）。

#### T12.4 移除 tmpfs 伪 /bin/bash
- 文件：`src/kernel/elf/hello_init.c`
- 做法：删除 `tmpfs_create_file("/bin/bash", hello_elf_data, ...)`，只保留 `/bin/hello` 注册。真实 bash 走 ext2。

#### T12.5 补齐 bash 最小运行 syscall（table.c + syscall.h + syscall.c）
- 在 table.c 注册（编号 → 实现）：
  - **10 mprotect**：返回 0（不真实改权限，最小验收）
  - **16 ioctl**：`TCGETS=0x5401` 返回 0（isatty=true，终端结构可不清写）；其他请求返回 -1
  - **4/5/6 stat/fstat/lstat**：填 glibc x86-64 `struct stat`（144 字节，重点 st_mode/st_size/st_nlink/st_uid/st_gid，其余 0）；fstat 从 fd 取 file_t（inode/type/size），stat/lstat 走 `vfs_open`+`ext2_inode_type/size` 探测；`vm_copy_to_user` 写回
  - **21 access**：`vfs_open` 探测，可打开返回 0，否则 -1
  - **33 dup2**：vfs_fd 表复制（关闭 newfd 原有，`vfs_fd_alloc_min` 不适用，直接数组复制 fd 槽）
  - **102/104/107/108 getuid/getgid/geteuid/getegid**：返回 0
  - **110 getppid**：从 `current_task` 读 `ppid`
  - **158 arch_prctl**：`ARCH_SET_FS=0x1002` 用 `wrfsbase` 写用户 fs.base；`ARCH_GET_FS=0x1003` 用 `rdfsbase`；其他返回 -1。前置：syscall_init 中 CR4 |= 0x10000（FSGSBASE），`syscall_entry.S` 无需改
  - **202 futex**：返回 0（单线程无等待语义）
  - **218 set_tid_address**：返回当前 pid
  - **257 openat**：`dirfd==AT_FDCWD(-100)` 时按路径走 `vfs_open`；其他 dirfd 返回 -1
  - **318 getrandom**：填充 n 字节伪随机（rdtsc 派生），返回 n
- 前置检查：确认 `syscall_dispatch` 对未注册编号默认返回 `-ENOSYS`；若 bash 崩溃报未知编号，用 gdb 或串口打印补漏（迭代式）。
- `syscall.h` 增加新函数原型；`MAX_SYSCALLS=256` 已够。

#### T12.6 ELF/动态链接联调
- 不动 loader.c/interp.c 主体。验证点：
  - ld-linux 映射（0x7F0000000000）与 auxv（AT_BASE/AT_PHDR/AT_ENTRY）正确传给 ld-linux 入口（rdi=AT 栈顶 rsp）
  - glibc 动态链接器会自行完成重定位（内核只映射 PT_LOAD）
  - 若 ld-linux 加载路径与 PT_INTERP 不一致（找不到解释器），按 T12.2 补符号链接
- 若失败，用既有手段定位：QEMU `-d int -D qemu_int.log` + gdb（`-s`，mingw64 gdb）断点 `elf_load_from_mem`/`sys_execve`。

#### T12.7 自动测试切换
- 文件：`src/kernel/kmain.c`
- 做法：第 221-225 行自动测试改为 `shell_execute("run /bin/bash")`（bash 退出后打印 Done 回内核 shell）。test_fork 可保留为手动命令。

---

## Assumptions & Decisions

1. **最小验收范围**（用户确认）：`bash-5.2$` 提示符 + 内置命令（echo/pwd/cd/exit）+ 单条外部命令（fork 同步模型足够）。管道/重定向/后台作业不在本次范围。
2. **glibc 动态 bash 为主方案**（用户原始要求）；下载失败/glibc 启动不可行时按 T12.1 回退链（Debian → Alpine musl → 静态 busybox，向用户说明）。
3. **构建用 build.ps1**；不修 Makefile（除非用户后续要求）。
4. 二进制从 Ubuntu/Debian 官方 pool 下载（不用 git）。
5. bash 动态依赖（libtinfo.so.6）一并注入；按实际 `readelf` 结果调整注入路径。
6. syscall 补齐按"glibc 动态启动最小集"实现；bash 运行时若报缺失编号，迭代补漏（syscall 默认 -ENOSYS 可观测）。
7. 内核大小/启动时间约束继续生效（kernel.flat < 80KB、boot < 2s）；新增 syscall 为小函数，风险低。

---

## Verification

1. **Task 11**：`.\build.ps1 clean`（无残留）→ `.\build.ps1 all`（0 错误）；kernel.flat < 80KB；fs.img 含 /bin/bash、/lib64/ld-linux-x86-64.so.2、/lib/x86_64-linux-gnu/libtinfo.so.6。
2. **QEMU 启动**：boot 到内核 shell，自动 `run /bin/bash`。
3. **bash 交互**：
   - 出现 `bash-5.2$` 提示符
   - `echo hello` → `hello`
   - `pwd` → `/`；`cd /bin` 后 `pwd` → `/bin`（验证 getcwd/chdir）
   - `exit` → 回到内核 shell 提示符
4. **外部命令**：`run /bin/hello`（tmpfs）与 `run /bin/test_fork`（ext2）仍正常。
5. **回归**：`mem`、`ps`、`power` 等内核命令不受影响。
6. 记录 bash 运行日志中未实现 syscall 编号并补齐，直至提示符稳定。

---

## Risks

- glibc 启动强依赖 arch_prctl(fs.base)/futex/openat/getrandom——wrfsbase 需 QEMU TCG 支持（新版支持）；若 #UD 则回退方案（arch_prctl 记录地址、sysret 前恢复，或转 musl bash）。
- 下载源网络不可达 → 换 deb.debian.org / 阿里云镜像。
- bash 运行时若还缺其他 syscall（如 getdents/readlink/pipe）→ 迭代补齐（syscall 默认 -ENOSYS 可观测）；完整交互（管道/后台）不在本次范围，bash 会以可接受方式报错或退化。
