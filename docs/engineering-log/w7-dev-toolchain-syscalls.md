# W7 计划：系统调用扩展至 ≥80 + 真实开发工具链 + 512MB 镜像（内核 v2.0.1）

## 1. 摘要

基于现有内核（当前已注册 72 项系统调用，含 shm/网络；实际业务调用 65 项），补齐至 **109 项**（新增 ~37 项），使系统能在 Ring 3 用户态运行 Ubuntu x86-64 真实二进制：**gcc、make、python3、vim、bash、coreutils**。同时：

- 建立 **banana(uid=1000) 普通用户 + root 初始化** 的权限模型
- 把 EXT2 镜像 `fs.img` 从 8MB 扩至 **512MB**（4096B 块、多块组）
- 新增 `tools/fetch_deps.py` 从 Ubuntu jammy 下载 .deb 并解包注入镜像
- 版本号升级为 **2.0.1**，`uname -r` 返回 `2.0.1-axion`
- 关键性能/稳定性：启动 < 200ms、`gcc hello.c` 编译 < 500ms、24h 浸泡无 Panic

## 2. 现状分析（已探索确认）

### 2.1 架构
- 轮询驱动、无中断；`syscall`/`sysretq` 进出 Ring 3；每任务独立 CR3（`vm_context_t`）
- 用户任务由 `wait4` **同步**运行（`child_runner` 汇编 + wait4 续栈），一次只跑一个子进程
- `syscall_entry.S`：入口快照用户寄存器到**全局** `syscall_user_rsp`/`syscall_user_ctx[16]`，切到全局 `syscall_kernel_top` 内核栈，dispatch 后按 exit/exec/signal 标记分支
- 调度：`sched.c` 有 task_table(64) + MLFQ(5 级) + `sched_tick/sched_next/sched_block/sched_wake`，但**无真实上下文切换**——用户任务切换不存在
- Shell `run` 命令经 `elf_load` + `sched_spawn_process` + `user_run` 同步执行，后台 job 是假的（登记即 Done）

### 2.2 关键缺口
| 缺口 | 影响 |
|---|---|
| 无任务上下文切换，wait4 一次只跑一个子进程 | `ls \| wc` 管道写满死锁；`sleep &` 后台不真跑 |
| 无 pipe/select/futex/nanosleep/clock_gettime | gcc/make/python3/vim 无法运行 |
| 无 getdents64 | 目录读取（ls、gcc 读头文件、python os.listdir）全挂 |
| 无 lseek/rename/chmod/umask/mkdir/rmdir/readlink/statfs | 工具链编译、文件操作缺基本能力 |
| `setuid/setgid` 无权限检查，uid/gid 单字段（无 euid） | 无法实现权限模型 |
| execve 不注入环境变量、auxv UID 硬编码 0 | bash 提示符、工具 HOME/PATH 异常 |
| 用户态 #PF 若未投递 SIGSEGV 会 hlt | python/gcc 探测内存即整机死锁 |
| `[tmpfs] lookup returned NULL` 大量刷屏 | 启动日志噪音 |
| ext2 驱动单块组（仅组 0 位图/inode 表） | 无法挂载 512MB 多组镜像 |
| mkfs 仅 8MB/1024B 块单组 | 无法生成 512MB 镜像 |

## 3. 决策记录（已与用户确认）

| # | 决策 | 内容 |
|---|---|---|
| D1 | 内核镜像大小 | 放宽至引导容量（约 503KB）为硬上限，不再强制 80KB；bootsect 现有容量校验保留 |
| D2 | 并发模型 | **协作式多任务调度**（协程式内核栈切换），替换 wait4 同步单子进程模型 |
| D3 | 镜像布局 | 保持双盘：`disk.img`（引导+内核）+ `fs.img` 扩至 **512MB**（4096B 块、多块组） |
| D4 | Ubuntu 版本 | **jammy 22.04**（x86-64）：gcc 11、glibc 2.35，依赖最少；.deb 数据压缩为 xz |
| D5 | 性能指标换算 | "/bin/true ×1000 < 300ns" 视为笔误，目标改为：**1000 次 fork+exec+exit < 1s** |

## 4. 实施阶段

### Phase 1（第 1 周）：协作式调度器 + 权限模型 + 进程/资源/文件批

#### 4.1 协作式多任务调度器（地基，D2）

**目标**：任意用户任务可在 syscall 中阻塞，控制权交给调度器运行其它任务；wait4 成为"等待子进程退出"的阻塞条件。

**设计（决策完整）**：
1. **每任务内核栈**：`syscall_entry` 改用 `current_task->kstack_top`（fork 时已分配），废弃全局 `syscall_kernel_top` 单一栈。
2. **每任务用户上下文**：新增汇编 `syscall_ctx_load/save(task)` 在任务切换时把全局 `syscall_user_rsp/syscall_user_ctx` 与 `task->fork_ctx`（已有 `user_regs_t`，扩容后含全部 16 寄存器）互搬。
3. **内核栈上下文切换**：新增汇编 `switch_context(from, to)`（保存 rbx/rbp/r12-r15/rsp/返回地址 到 `from->kctx`，加载 `to->kctx` 并 ret）。任务阻塞 = `sched_block_and_switch(cond)`：置 BLOCKED、记录条件、`switch_context` 回调度器。
4. **调度器主体**：`kmain` 主循环与 `sched_next()` 已有就绪队列。扩展为：取出就绪任务后，若 `t->kctx.valid` 则 `switch_context` 恢复（从阻塞点继续），否则首次运行走 `enter_user(entry, rsp)`。任务 exit 经 `.Lreturn_to_kernel` 返回调度器（复用现有 user_ret_* 机制，改为按任务保存）。
5. **阻塞条件与唤醒**：`sched_block(kind, arg)` 登记；对端推进时 `sched_wake_cond(kind, arg)` 置 READY 并入队。种类：`PIPE_RD(fd)`/`PIPE_WR(fd)`/`FUTEX(addr)`/`TIME(deadline)`/`SELECT(fds)`/`CHILD(pid)`。
6. **wait4 重构**：不再同步跑单子进程，改为 `sched_block(CHILD, pid)`；子进程 zombie 时 wake。回收逻辑不变（REAPED）。

**修改文件**：
- `src/kernel/sched/sched.h`：task_t 增加 `kctx{valid, rsp, rbx, rbp, r12-r15}`、`wait_kind`、`wait_arg`、`cwd[96]`、`umask`、`euid/egid`、`rlimit_cur[16]/rlimit_max[16]`、`altstack{base,size,onstack}`、`block_nr/block_args[6]`
- `src/kernel/sched/sched.c`：`sched_block_and_switch`、`sched_wake_cond`、`sched_resume`、`sched_next` 完善（跳过 BLOCKED）
- `src/kernel/syscall/syscall_entry.S`：per-task 内核栈；`switch_context`；`syscall_ctx_save/load`
- `src/kernel/syscall/syscall.c`：删除全局栈假设，改为 `current_task->kstack_top`
- `src/kernel/syscall/table.c`：`sys_wait4` 重构为阻塞等待
- `src/kernel/kmain.c`：主循环调度逻辑完善（switch_context 恢复阻塞任务）
- `src/kernel/core/shell.c`：`user_run` 调用改走调度器；后台 job 真正入队

**回归关卡**：`test_w6_e2e.ps1` 13/13 保持通过；新增手测 `run /bin/sleep 1` 前台挂起 1s 后返回（临时用 `sleep` 测试 ELF 或内核命令验证）。

#### 4.2 权限模型（banana uid=1000）

- task_t：`uid`(real) + `euid` + `gid` + `egid` 四字段；`sched_spawn_process/sched_add` 默认取全局 `g_default_uid/g_default_gid`。
- `users_init()` 已解析 `/etc/passwd` → 增加解析 `banana:` 行，设置 `g_default_uid=1000, g_default_gid=1000`。
- **drop 时机**：kmain 初始化完成后、auto-run bash 前生效——新 spawn 的用户任务默认 uid=1000（root 仅用于内核初始化阶段）。
- `setuid(105)`/`setgid(106)` 加权限检查：`euid==0` 可设任意值；否则仅允许设 `uid/euid`（等于自身）。设后 uid=euid=(suid)=new。Linux 语义。
- `getuid(102)/geteuid(107)/getgid(104)/getegid(108)` 返回对应字段。
- `elf_build_user_stack` auxv：`AT_UID/EUID/GID/EGID` 从 `current_task` 取（不再硬编码 0）。
- ext2 权限检查改用 euid；root(0) 绕过（已有逻辑核对修正）。
- 目录权限：镜像中 `/`、`/bin` 等由 root 拥有 755，banana 写入返回 -EACCES。

**修改文件**：`sched.h/sched.c`、`syscall/table.c`、`elf/loader.c`、`fs/vfs.c`(权限检查用 euid)、`core/shell.c`。

#### 4.3 进程/资源/文件批系统调用（新增注册）

| nr | 名称 | 语义 | 实现要点 |
|---|---|---|---|
| 112 | setsid | 新会话：pgrp=pid，清除控制终端 | task_t 加 `sid` 字段 |
| 127 | getpgrp | 返回 pgrp | 同 getpgid |
| 97 | getrlimit | 返回 rlimit 表 | task_t 存 rlimit；默认 {RLIM_NOFILE:64, RLIM_STACK:8M, 其它 INF} |
| 75 | setrlimit | 设置 rlimit | 校验 cur<=max |
| 302 | prlimit64 | 改真 | 复用 rlimit 表 |
| 98 | getrusage | 填零 `struct rusage` | 直接 memset 0 返回 |
| 8 | lseek | 按 SEEK_SET/CUR/END 改 f->offset | vfs_lseek 已存在，包一层 |
| 81 | fchdir | 按 fd 对应文件路径 chdir | 复用 chdir 路径解析 |
| 82 | rename | ext2 目录项搬移 | 新增 `ext2_rename`（同组：改目录项名/移项） |
| 83 | mkdir | 建目录 | `ext2_mkdir` 已存在 + 权限检查 + `mkdirat` 别名 |
| 84 | rmdir | 删空目录 | 新增 `ext2_rmdir`（校验空目录、. / .. 处理） |
| 85 | readlink | 读符号链接内容 | `ext2_read_inode` + `i_blocks` 直读 fast_symlink |
| 90 | chmod | 改文件 mode | 新增 `ext2_chmod`（写 inode i_mode 低 12 位）+ 权限检查 |
| 268 | fchmodat | 按 dirfd chmod | 复用到 chmod 路径 |
| 95 | umask | 设/取任务 umask | task_t 字段 |
| 76 | truncate | 截断文件 | `ext2_truncate` 已存在，按路径 |
| 77 | ftruncate | 按 fd 截断 | 同上按 fd |
| 137 | statfs | 返回 `struct statfs` | 由 ext2_sb 填：bsize/block/ino 计数/name[16]="ext2" |
| 138 | fstatfs | 按 fd statfs | 同上 |
| 99 | sysinfo | 填 `struct sysinfo` | uptime/总内存/空闲内存（mm.c 统计） |

**修改文件**：`syscall/table.c`（全部新增）、`fs/ext2.c`（rename/rmdir/chmod）、`fs/vfs.c`。

**验证**：内核 shell 手测 `mkdir /tmp/d1`、`rmdir /tmp/d1`、`cat /etc/passwd` 输出 banana 行、`ls -l` 权限位。

### Phase 2（第 2 周）：I/O 管道 + 终端 + 信号/时间批 + execve 环境 + 稳定性修正

#### 4.4 管道与 I/O 批

| nr | 名称 | 语义 | 实现要点 |
|---|---|---|---|
| 22 | pipe | 建管道，写 [0]=读端 [1]=写端 | 新文件 `fs/pipe.c`：`pipe_t{环形缓冲 32KB, head, tail, readers, writers, rd_wait, wr_wait}`；fd 对 |
| 293 | pipe2 | pipe + flags(O_NONBLOCK/O_CLOEXEC) | 同上 |
| 7 | poll | 多 fd 就绪检查/阻塞 | stdin(fd0)、pipe、普通文件(fd 恒就绪)；超时由 `wait_kind=SELECT` 阻塞；支持 POLLIN/POLLOUT/POLLERR/HUP；重入安全 |
| 19 | readv | 聚散读 | 遍历 iovec 调 vfs_read |
| 217 | getdents64 | 读目录项 | `ext2_read_dir` 回调改造成填充 `struct linux_dirent64`；支持 tmpfs 目录（`tmpfs_list` 已有）；缓冲不跨读返回 EINVAL 处理 |
| 78 | getdents | 旧 32 位 dirent（可选） | 同上转换 |

阻塞语义：pipe 读空/写满 → `sched_block(PIPE_*)`；对端读写时 `sched_wake_cond`。O_NONBLOCK（fcntl F_SETFL 已支持）时满/空直接返回 -EAGAIN。管道的 fd 在 fork 时随 fd_table 深拷贝（已有）。

**修改文件**：新增 `src/kernel/fs/pipe.c`、`fs/pipe.h`；`syscall/table.c`；`fs/ext2.c`(目录迭代导出)、`fs/tmpfs.c`(目录迭代导出)、`fs/vfs.c`(fd_table 扩至 256)。

#### 4.5 终端与 job control

- **tcgetattr/tcsetattr**：已通过 ioctl(TCGETS/TCSETS) 实现（devfs termios 状态），核验 c_lflag/c_oflag 写入完整（echo、icanon）。
- **tcsetpgrp/tcgetpgrp**：ioctl(TIOCSPGRP/TIOCGPGRP) 已由 devfs `tty_get/set_pgrp` 支撑——核验/补全，确保 bash job control 不再告警。
- **getpgrp/setsid/setpgid** 与 tty pgrp 联动：bash 前台作业组设置后，`tcsetpgrp` 返回 0。
- **修改**：`fs/devfs.c`、`syscall/table.c`(sys_ioctl 完整映射 TIOC* 到 devfs)。

#### 4.6 信号与时间批

| nr | 名称 | 语义 | 实现要点 |
|---|---|---|---|
| 131 | sigaltstack | 设/取备用信号栈 | task_t `altstack` 字段；signal delivery 时 SA_ONSTACK 用 altstack |
| 111 | rt_sigpending | 返回待决且被阻塞的信号 | 真实现：`sig_pending & sig_blocked` 填用户 sigset |
| 128 | rt_sigtimedwait | 等待信号（带超时） | 无信号立即 -EAGAIN；有信号消费最低位并返回 |
| 35 | nanosleep | 挂起 n 秒（余量写回） | `sched_block(TIME, deadline)`；唤醒后重算 remaining |
| 36/38 | getitimer/setitimer | 间隔定时器（ITIMER_REAL） | task 字段 + 调度循环轮询到期信号 SIGALRM |
| 37 | alarm | 一次性定时 | 同 setitimer 简化 |
| 114 | clock_getres | 返回精度 1ms | 0x1000 纳秒（timer_ms 粒度） |
| 228 | clock_gettime | CLOCK_REALTIME/MONOTONIC | MONOTONIC=timer_ms()*1ms；REALTIME=单调+系统启动基准 |

**修改文件**：`syscall/signal.c`(sigaltstack/rt_sigpending/rt_sigtimedwait 投递用 altstack)、`syscall/table.c`(时间批)、`sched.c`(TIME 条件轮询唤醒)。

#### 4.7 同步与系统批

| nr | 名称 | 语义 | 实现要点 |
|---|---|---|---|
| 202 | futex | WAIT/WAKE 真实现 | 单地址等待队列：`wait_kind=FUTEX, arg=addr`；WAKE(nr) 唤醒至多 nr 个；WAIT 检查 `*uaddr==val` 否则立即返回 EAGAIN |
| 157 | prctl | PR_SET_NAME → task->name；其余返回 0 | python 线程命名需要 |
| 28 | madvise | no-op 返回 0 | gcc/python 惯例调用 |
| 140/141 | getpriority/setpriority | 返回 0 / no-op | make -n 需要 |
| 58 | vfork | 路由到 sys_fork | glibc posix_spawn 可能用 |
| 218 | set_tid_address | 已实现 ✓ | — |

#### 4.8 execve 环境注入 + 用户态故障处理 + VFS 顺序

- **execve 默认环境**：`sys_execve` 构造 envp：`PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin`、`HOME=/home/banana`、`TERM=xterm`、`SHELL=/bin/bash`、`USER=banana`、`LOGNAME=banana`、`PWD=<cwd>`。用户提供 envp 时优先合并（用户同名键覆盖）。
- **VFS 查找顺序**：`vfs_open_mode` 改为 devfs → procfs → sysfs → **ext2 → tmpfs**（tmpfs 最后，消除 `[tmpfs] lookup returned NULL` 噪音；tmpfs.c 内部静默返回 NULL）。
- **用户态 #PF → SIGSEGV**：`idt.c` 异常处理对用户态故障（#PF/#GP/#UD/#DE）默认投递 SIGSEGV/SIGILL（复用 `signal_deliver_now` + `exc_kill_user`）；无 handler → 终止进程（kill 默认动作）回父进程 wait4，**绝不 hlt 整机**。核对 idt.S 的 vector 分发路径。
- **rep movsb**：`vm.c` 新增 `memcpy_user(void* dst, const void* src, size_t)` 用 `rep movsb`（大块 ≥64B 分支），用于 elf_load 文件读、exec 参数拷贝、pipe 传输、readv/writev、`vm_copy_to/from_user` 大块路径。
- **`-ENOENT` 静默**：文件不存在仅返回 -ENOENT，无内核日志（tmpfs 打印删除）。

**修改文件**：`syscall/table.c`、`fs/vfs.c`、`fs/tmpfs.c`、`core/idt.c`、`mm/vm.c`、`elf/loader.c`、`syscall_entry.S`(无需，故障在 idt 路径)。

**验证**：bash 内 `echo hello | tr a-z A-Z`（busybox 风格内建或 `cat | cat`）、`gcc` 之前的冒烟：`ls /`、`cat /etc/passwd`、`mkdir /tmp/x && cd /tmp/x && pwd`、`python3 -c "print('ok')"`。

### Phase 3（第 3 周）：512MB 镜像 + 工具链注入 + 性能/稳定性 + 回归 + 文档

#### 4.9 多组 EXT2 驱动（内核侧）

**现状**：`ext2.c` 位图/inode 表/分配全假设单块组（组 0）。

**改造**：
- `ext2_group_desc(group)`：GDT 多块读取（组号→GDT 块偏移，`s_first_data_block+1` 起，entries_per_block 计算已有，去掉单组假设）。
- `ext2_read_inode/write_inode`：inode → `(group, index_in_group)` → 该组 inode 表块地址。
- `ext2_alloc_inode/alloc_block/free_*`：遍历各组 gd 找空闲，更新对应组位图 + gd 计数 + 超级块计数（多组合计）。
- 块大小动态：`ext2_blk_size` 已按 `s_log_block_size` 计算；核对 4096B 时 `s_first_data_block=0`、sectors_per_block=8 的路径（read/write block、目录项 rec_len 计算按块大小）。

**修改文件**：`src/kernel/fs/ext2.c`、`src/kernel/fs/ext2.h`。

#### 4.10 镜像制作（工具侧）

**`tools/ext2_mkfs.py` 重构**：
- 参数化：`--size 512M --block-size 4096 --inodes-per-group 8192`。
- 512MB/4096B → 131072 块、4 块组、32768 块/组、8192 inode/组、inode 表 256 块/组。
- 多组 GDT 布局（4 组 × 32B = 1 块）；每组位图/inode 表独立。
- 保留现有 manifest 注入 + 目录 materialize + 符号链接能力；文件 > 双间接块继续支持。

**新增 `tools/fetch_deps.py`（Windows 原生）**：
- 输入：顶层包列表 `bash coreutils gcc make python3 vim libc6 libc6-dev binutils libtinfo6 ncurses-base libgcc-s1 libstdc++6 linux-libc-dev`。
- 从 `http://archive.ubuntu.com/ubuntu/dists/jammy/main/binary-amd64/Packages.xz`（+universe）解析依赖并递归展开（剥离 `Depends: (=ver)` 版本约束，忽略 Recommends/Suggests）。
- 下载 .deb 到 `tools/debs/`（断点续传、校验 sha256 可选）。
- 解包：`ar t` 取 `data.tar.xz`（Python `lzma` 解压），展开 `etc/ usr/ lib/ bin/ sbin/` 到 `tools/rootfs/`。
- 产物：生成 `tools/fs_manifest.txt`（`rootfs/<path>:<img_path>`，含 `usr/bin/gcc`、`usr/bin/as`、`usr/bin/ld`、`usr/lib/gcc/x86_64-linux-gnu/11/cc1`、动态链接器、全部 .so、python3 stdlib、vim-runtime、terminfo 等）。
- 保留旧 `unpack_deb.py` 的 zstd 支持（jammy 用 xz，新增分支）。

**配置文件注入（镜像内）**：
- `/etc/passwd`：`root:x:0:0:root:/root:/bin/bash` + `banana:x:1000:1000:banana:/home/banana:/bin/bash`
- `/etc/group`：root、banana 组
- `/etc/nsswitch.conf`：`passwd: files` / `group: files`（glibc nss_files 需在）
- `/etc/hostname`：`axion`
- `/etc/profile`：`export PATH=...`；`/etc/bash.bashrc`：`PS1='\u@\h:\w\$ '`（产生 `banana@axion:~$`）
- `/home/banana/.bashrc`、`/home/banana/.profile`
- `/etc/ld.so.conf`：默认路径（/lib/x86_64-linux-gnu 已覆盖，无需 cache）

**修改文件**：`tools/ext2_mkfs.py`、`tools/fetch_deps.py`(新)、`tools/unpack_deb.py`、`tools/fs_manifest.txt`、`build.ps1`(fs.img 生成参数改为 512MB，校验 >300MB)。

#### 4.11 版本号与回归

- `sys_uname`：release 改 `"2.0.1-axion"`；banner 改 `Axion-Ban Kernel v2.0.1`。
- 回归：`test_w6_e2e.ps1` 13/13 保持（fs.img 内容向后兼容：保留 /bin/bash、/bin/cat、/bin/rm、/bin/hello、test_*）。
- 新增验收脚本 `test_w7_acceptance.ps1`（见第 5 节）。
- 新增压力脚本 `test_w7_stress.ps1`：随机命令序列 100 条 + 24h soak（qemu 重启检测）。

## 5. 验收标准（对应需求清单）

| 验收项 | 通过标准 | 脚本 |
|---|---|---|
| bash 无 job control 警告 | 启动日志无 "no job control" | test_w7_acceptance.ps1 |
| 提示符 | `banana@axion:~$` | 日志正则匹配 |
| gcc 编译 | `gcc hello.c -o hello` 成功，`./hello` 输出正确 | 同上 |
| python3 | `python3 -c "print('ok')"` → `ok` | 同上 |
| make / vim | `make -v`、`vim --version` 有输出（vim 用 `-es` 冒烟或 `+q`） | 同上 |
| 权限拒绝 | banana 写 `/`、`/bin` 返回 Permission denied（-EACCES） | 同上 |
| 持久化 | `echo x > /tmp/test.txt` → 重启后 `cat` 内容保留 | 复用 write_phase1/phase2 |
| 性能 | 启动 < 200ms；`gcc hello.c` < 500ms；/bin/true×1000 < 1s | 日志时间戳 |
| 稳定性 | 24h 无 Panic；随机命令 100 次无崩溃 | test_w7_stress.ps1 |
| syscall 数 | 注册表 ≥ 80（目标 109） | 内核 `syscall_count` 或代码审查 |
| 版本 | `uname -r` = `2.0.1-axion` | 日志 |

## 6. 交付物

内核源码（v2.0.1）、`build.ps1`、`tools/ext2_mkfs.py`、`tools/fetch_deps.py`（+unpack_deb.py 扩展）、`fs.img`(512MB)、`disk.img`、`tools/fs_manifest.txt`、验收报告 `docs/验收报告.md`、快速启动指南 `docs/快速启动指南.md`。

## 7. 风险与缓解

| 风险 | 缓解 |
|---|---|
| 协作调度器改动波及 wait4/信号/exec 全链路 | Phase 1 每步过 test_w6_e2e 13/13 回归；阻塞 syscall 全部写成重入安全 |
| gcc 依赖 binutils(as/ld)+头文件+libgcc 等深层依赖 | fetch_deps.py 递归 Depends；先做 bash+coreutils 最小集再逐层加 |
| TCG 模拟下 gcc 编译 < 500ms 偏紧 | 先达功能，再优化（rep movsb、镜像去校验和？不——保持 ext2 完整性）；若超时向用户报告实测值 |
| TCG 下 AP 核 INIT-SIPI 抖动 | 已知问题：不影响 BSP 路径；压力测试用 BSP 单核热循环验证逻辑 |
| 512MB 镜像 QEMU 读写慢 | 仅在需要时重建；测试用现有 8MB 镜像做大部分回归，512MB 用于验收 |
| network 下载 .deb 需要外网 | fetch_deps.py 支持已下载缓存目录离线复用；失败给出明确提示 |

## 8. 实施顺序（建议 commit 粒度）

1. Phase 1.1 协作调度器骨架 + wait4 重构 → 回归
2. Phase 1.2 权限模型（uid/euid/setuid/auxv）→ 回归
3. Phase 1.3 进程/资源/文件批（4.3 表）→ 回归
4. Phase 2.1 pipe/select/readv/getdents64 → 手工管道测试
5. Phase 2.2 终端 job control + 时间/信号批 + futex/prctl 等
6. Phase 2.3 execve 环境注入 + VFS 顺序 + #PF→SIGSEGV + rep movsb + ENOENT 静默
7. Phase 3.1 内核多组 EXT2 驱动（挂 512MB 镜像冒烟）
8. Phase 3.2 fetch_deps.py + ext2_mkfs.py 512MB + 工具链注入 + 配置
9. Phase 3.3 性能/稳定性/回归/文档/版本号 2.0.1
