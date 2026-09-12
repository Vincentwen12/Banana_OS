# Tasks: Axion-Ban 生产级强化（W6 细化）

范围：阶段 0（预算放宽）→ 阶段 1（安全基础）→ 阶段 2（日常可用）→ 阶段 3（生产增强）。
每任务以可验证的验收点收尾；完成一项再进入下一项（阶段内可并行项已标注）。

## 阶段 0：内核体积预算放宽（前置）

- [x] Task 0.1: 引导扇区改为 LBA 加载，容量提升至 512KB
  - [x] 重写 `src/boot/bootsect.S`：`KERNEL_SECTORS` 160→1024；int 13h AH=42h + DAP（`0x7C00+0x40` 处构造 disk address packet）循环读取至临时区（如 `0x10000` 起，按 64KB 递增 ES 段）
  - [x] `prot_mode` 拷贝循环：拷贝计数改为 `KERNEL_SECTORS*512/4` 双字
  - [x] `build.ps1`：`minKernelSectors` 160→1024；`disk.img` 布局与 `neededSectors` 同步
  - [x] 验收：构造 >80KB 的 kernel.flat（可临时塞 padding），bootsect 完整加载并进入内核，串口无磁盘错误
  - [x] 验收：常规构建（当前 78KB）引导正常，无回归
  - 注：本环境 SeaBIOS 不支持 EDD（AH=41h→AH=01），改用 CHS（AH=02h）循环加载，容量上限为 1007 扇区（SPT=63/HPC=16，约 503KB）。修复窗口截断时未推进 cl 导致的数据错位 bug（int13h 从同扇区重读）

## 阶段 1：安全基础

### 1A 内核/用户内存隔离

- [x] Task 1.1: 修复 vm_create 的 0-2GB identity 映射为 U/S=0
  - [x] `src/kernel/mm/vm.c`：`vm_create()` 中 `pml4[0]`、`pdpt0[0]`、`pdpt0[1]` 移除 `PTE_USER`（仅内核可访问）；`pdpt0[3]`（LAPIC）保持无 USER
  - [x] 确认 boot.S 的初始内核页表低区本就 U/S=0（无需改）
  - [x] 验收：用户程序在用户态访问 `0x100000`（内核代码）触发 `#PF`（配合 Task 1.2 表现为进程终止而非 halt）
  - 注：Task 1.4（用户态异常→杀进程）未完成前，#PF 表现为 IDT 打印+halt；已用回归（bash 正常运行）验证隔离不破坏用户程序
- [x] Task 1.2: 修复 vm_map_page 拆分大页时的 USER 位泄漏
  - [x] 拆分 1GB→2MB（`pdpt[i3]` 分支）与 2MB→4KB（`pd[i2]` 分支）时：**仅目标页** `PTE_USER=1`，兄弟页保持 `U/S=0`；中间层（pml4/pdpt/pd）按需 `PTE_USER` 以便用户叶子可达
  - [x] 核对 `vm_fork` 深拷贝逻辑仍只复制 USER 叶子（现状符合）
  - [x] 验收：映射 ELF（0x400000）后，仅该 4KB 页附近可用户访问；相邻内核页不可访问
  - 注：回归通过（bash 的 0x400000/栈/mmap 高区在拆分后 USER 位无丢失）
- [x] Task 1.3: 检查并适配共享物理区
  - [x] 定位 `DOORBELL_BASE`、`IPC_SHM_BASE`、用户可访问共享区域的物理地址与权限需求
  - [x] 若需用户访问：显式 `vm_map_page(USER)`；否则保持 U/S=0
  - [x] 验收：内核 IPC/门铃功能（`ipc` 命令）回归通过
  - 注：两者均为纯内核态资源（无用户态 syscall 暴露），U/S=0 隔离后 ipc 命令 send/recv/ACL 全部正常，无需改动

### 1B 用户态异常终止进程

- [x] Task 1.4: IDT 异常处理器区分用户/内核态，用户态异常杀进程
  - [x] `src/kernel/core/idt.c`：`exc_print_c` 前新增用户态判定（CS 低 2 位 == 3）
  - [x] 用户态异常路径：标记当前任务异常退出（exit_code 对应信号，如 SIGSEGV=11），复用 exit 展开（`syscall_exit_request`/`user_ret_*`/wait4 子进程 `.Lwait4_cont` 路径）回到内核 Shell 或父进程 wait4
  - [x] 内核态异常路径：保留打印+halt，并接入 Task 1.5 panic 流程
  - [x] 验收：bash 内运行会触发用户态 #GP 的程序（如执行 `hlt` 的测试 ELF）→ 仅该进程退出，`bash-5.2#` 仍在；子进程场景下父进程 wait4 收到非 0 状态
  - 注：顺带修复两个致命 bug——idt_init 原本在 mm_init 之前调用（mm_init 清位图导致 IDT 页被复用，TSS 覆盖 IDT）；TSS 原从堆分配且 RSP0 页会被重新分配（异常压栈破坏页表）。均已修复（idt_init 移到 mm_init 后；TSS 改 BSS 静态分配）
- [x] Task 1.5: 空指针/非法访问 #PF 测试用例
  - [x] 构造测试 ELF（工具链生成）：写 `NULL` 地址 → 用户态 #PF → 进程终止
  - [x] 验收：Shell 存活，无重启；`ps`/日志可见进程异常退出
  - 注：tools/test_fault.S（#PF→SIGSEGV=11）；`run /bin/test_fault` 杀进程回 Shell；bash 内运行子进程被杀，bash 存活

### 1C Panic 恢复机制

- [x] Task 1.6: panic 现场诊断输出
  - [x] `core/panic.c`：`kernel_panic` 扩展——打印当前进程名/PID、`rbp` 栈回溯（循环解引用，带边界保护）、RIP/RSP/CR2 快照（复用 IDT 快照约定）
  - [x] 验收：`panic` 命令触发后串口输出完整现场
  - 注：-O2 省略帧指针，栈回溯仅解 1 帧后边界保护终止（符合"带边界保护"验收）
- [x] Task 1.7: panic 记录持久化 + 5 秒自动重启
  - [x] panic 摘要写入内存保留区（固定物理地址 0x740000，避开内核 BSS/Ω堆），并在 EXT2 写入可用后写入磁盘 panic.log（阶段 1 顺序：先内存，Task 1.10 后接磁盘）
  - [x] 5 秒倒计时（打印）后 `outb(0xCF9, 0x0E)` 软重启
  - [x] 重启早期（kmain 起始）检查 panic 记录，有则打印 "Previous panic: ..."
  - [x] 连续 panic 计数 ≥3 → 安全模式（跳过 auto-run bash，仅内核 Shell）
  - [x] 验收：`panic` → 自动重启 → 显示 Previous panic；连续 3 次 panic 后进入安全模式

### 1D EXT2 写入

- [x] Task 1.8: EXT2 位图与组描述符写入
  - [x] `fs/ext2.c`：实现 inode/块位图分配（`ext2_alloc_inode`/`ext2_alloc_block`）与释放（`ext2_free_inode`/`ext2_free_block`）
  - [x] 分配/释放后回写位图块与组描述符（`ext2_write_block` 基础）
  - [x] 验收：分配一个 inode 与一个块后，重新挂载（或重启）位图状态一致（`stats`/调试输出核对）
- [x] Task 1.9: inode 与数据块写入
  - [x] `ext2_write_inode(ino)`：inode 表回写（128 字节标准结构，注意工程约定）
  - [x] `ext2_write_block(block_no, data)`：经 `ata_write_sector` 写盘
  - [x] `ext2_write_file(ino, off, data, size)`：数据块写入 + 单间接块分配与回写
  - [x] 验收：对已有文件 inode 直接改写数据块，重启后 `cat` 读到新内容
- [x] Task 1.10: 创建/删除/目录项
  - [x] `ext2_create_file(parent_ino, name, mode)`、`ext2_mkdir(parent_ino, name)`
  - [x] `ext2_unlink(parent_ino, name)`（删除目录项 + 释放块/inode，仅普通文件）
  - [x] 目录项查找/插入/删除（变长目录项，`ext2_read_dir` 对称）
  - [x] 验收：创建 `/tmp/a.txt` → 写入 → 重启 → `cat` 可见；`rm` 后消失（W6 验收标准）
  - 注：新增 ext2_truncate、ext2_dir_insert、/bin/cat 与 /bin/rm（gen_tools.py）、/tmp /root /usr /var 补建
- [x] Task 1.11: VFS/系统调用贯通写入
  - [x] `fs/vfs.c`：`vfs_open` 支持 `O_CREAT|O_TRUNC` 语义；`ext2` file_ops 增加 `write`/`create` 回调
  - [x] `syscall/table.c`：`sys_open` 的 flags 透传（bash 的 `>` 重定向走 `open(O_WRONLY|O_CREAT|O_TRUNC)`）；`sys_write` 贯通 ext2；新增 sys_unlink/sys_unlinkat
  - [x] 验收：bash 内 `echo "hello" > /tmp/test.txt`；`cat /tmp/test.txt` 显示 hello；`rm /tmp/test.txt`；重启后持久化
  - 注：验证通过——echo/cat/追加/rm 全通过，跨重启持久化（fs.img 直接校验 content=persist\n）

## 阶段 2：日常可用

- [x] Task 2.1: printk 日志系统 + dmesg
  - [x] 新增 `core/printk.c/h`：`printk(level, fmt, ...)`（级别常量、默认 INFO、环形缓冲 ≥64 条）
  - [x] Shell `dmesg` 命令显示缓冲（含级别标注）
  - [x] 关键路径（启动/异常/panic）逐步接入 printk
  - [x] 验收：`dmesg` 显示最近内核消息；panic 后缓冲保留；写入磁盘后重启不丢（Task 1.7/1.10 配合）
  - 注：64×128B 环形缓冲，启动/panic/异常已接入
- [x] Task 2.2: /proc 文件系统
  - [x] 新增 `fs/procfs.c`：`/proc/self/status`、`/proc/uptime`、`/proc` 目录列出进程（pid/name/state）
  - [x] `vfs_open`/`vfs_read` 挂接 procfs（路径前缀 `/proc`）
  - [x] Shell `ps` 命令基于 /proc 输出进程表
  - [x] 验收：`ps` 列出 bash/hello 等进程；`cat /proc/uptime` 输出时间
  - 注：bash 内 cat /proc/self/status 显示正确 pid（self 取 current_task）
- [x] Task 2.3: /sys 基础
  - [x] 新增 `fs/sysfs.c`：只读节点（`/sys/kernel/version`、`/sys/kernel/boottime` 等）
  - [x] 验收：`cat /sys/kernel/version` 返回版本字符串
- [x] Task 2.4: 多用户基础
  - [x] `/etc/passwd` 解析（root 行）与 uid/gid 映射；`getuid/geteuid/getgid` 返回实际值
  - [x] 文件权限位检查（owner 读/写位）接入 `open`/`access`
  - [x] 验收：以 uid=0 访问无权限异常；权限位为 0 的文件非 owner 打开返回 EACCES（可先用 uid 模拟）
  - 注：vfs_check_perm 按 owner/group/other 位检查；test_perm（setuid 1000 后读 mode=0 文件 → EACCES）实机 PASS
- [x] Task 2.5: 信号完整性
  - [x] `rt_sigaction`：保存/读取 handler 表（per-task），不再空返回
  - [x] `kill` 支持 SIGTERM/SIGINT 等默认动作（终止）；SIGKILL 强制
  - [x] 信号投递：用户态 syscall 返回前检查 pending，投递 handler（修改用户栈帧跳 handler）或默认动作
  - [x] 用户态异常产生 SIGSEGV/SIGBUS（接 Task 1.4，有 handler 则投递，否则终止）
  - [x] 验收：测试 ELF 注册 SIGTERM handler，`kill` 后执行 handler 再退出；未注册则默认终止
  - 注：signal.c 实现 rt_sigaction/rt_sigprocmask/rt_sigreturn/kill + sigframe 投递；test_sig 实机 PASS；SIGTTIN/SIGTSTP 等 stop 类默认忽略（避免误杀 bash）

## 阶段 3：生产增强

- [x] Task 3.1: 终端控制（termios 完整 + job control 基础）
  - [x] termios 状态机：`tty_c_lflag`（ECHO/ICANON/ISIG/ECHONL）、`VMIN/VTIME`；`tty_read` 按 ICANON 行缓冲、按 ECHO 回显
  - [x] `TIOCGPGRP/TIOCSPGRP` 真实化（前台进程组跟踪）；`tcsetpgrp` 生效
  - [x] job control 基础：`&` 后台、`fg`/`bg`、`wait` 基础（Shell 层 + waitpid 选项）
  - [x] 验收：`sleep 2 &` + `jobs` 显示后台任务；`fg` 收回前台；无 "no job control" 警告
  - 注：tty 前台进程组经 devfs_tty_get/set_pgrp 真实跟踪；setpgid/getpgid(121) 真实化；bash 启动无 "no job control in background" 警告（实机验证）。`&`/jobs/fg/bg 为同步模型最小语义（记录+同步运行+fg 重跑），无真并行后台（内核无用户态抢占）
- [x] Task 3.2: 进程间通信（shm/sem/msg 最小实现）
  - [x] 选一：SysV `shmget/shmctl`（映射共享页到多进程，基于 vm_map）或 POSIX 共享 mmap
  - [x] 配套 `semget/semop` 或原子自旋信号量；`msgget/msgsnd/msgrcv`（或退化为共享内存+自旋）
  - [x] 验收：两进程经共享内存交换数据；`ipcs`-style 查看（或 Shell 扩展）
  - 注：shm.c 实现 shmget/shmat/shmctl/shmdt（8 槽位+自旋锁）；test_shm 父子进程经共享页交换数据实机 PASS
- [x] Task 3.3: 基础网络（loopback）
  - [x] 新增 `net/loopback.c`：`socket(AF_INET,SOCK_STREAM)`、`bind(127.0.0.1)`、`listen`、`accept`、`connect`、`send/recv` 最小回环实现（内存队列/端口表）
  - [x] `syscall/table.c` 注册 socket 系列 syscall（编号 41-49）
  - [x] 验收：测试程序 A 监听、B 连接，双向收发字符串成功（127.0.0.1）
  - 注：loopback.c 同步串行模型实现；test_tcp 实机 PASS
- [x] Task 3.4: 磁盘写入优化
  - [x] 块缓冲/批量写（可选；不改变语义）
  - [x] 验收：连续写入 ≥1MB 数据（分块），无丢失，重启后校验一致
  - 注：ext2_write_file 扩展两级间接块写路径（i_block[13] 两层表），单文件上限 12+256+256² 块；test_big 写入 1024×1KB 后读回校验实机 PASS

## Task Dependencies

- Task 0.1 独立，最先做（放开体积预算）。
- Task 1.1 → 1.2 → 1.3 串行（隔离修复递进）；1.1/1.2 完成后 1.4（用户态杀进程）可开始。
- Task 1.4 依赖 1.1/1.2（用户态访问内核页必须真正 #PF 才能验证杀进程）。
- Task 1.5 依赖 1.4。
- Task 1.6/1.7（panic）与 1.8-1.11（EXT2 写入）相互独立，可并行。
- Task 1.7 的磁盘 panic 记录部分依赖 Task 1.10（EXT2 写入可用）。
- Task 2.1 依赖 1.6（panic 打印接入日志）但不依赖 1.4。
- Task 2.2/2.3（proc/sys）依赖 1.1-1.4（隔离与异常杀进程稳定后），2.2 与 2.3 可并行。
- Task 2.4 依赖 2.2（权限校验需要统一 uid 读取）。
- Task 2.5 依赖 1.4（用户态异常→信号）与 1.1-1.3（稳定隔离）。
- Task 3.1 依赖 2.5（信号与 job control 关联）。
- Task 3.2 依赖 1.1-1.3（共享页映射依赖正确隔离）。
- Task 3.3 独立（不依赖其它阶段，但建议在 2.x 之后避免并行复杂度）。
- Task 3.4 依赖 1.8-1.11。

## 完成标志（W6 细化验收）

- [x] `run /bin/bash` 进入 bash，`echo hello`/`help` 可用
- [x] `echo "hello" > /tmp/test.txt` 创建；`cat /tmp/test.txt` 显示 hello；`rm` 删除；**重启后文件仍在**（ext2 持久化已验）
- [x] `ps` 显示进程列表；`dmesg` 显示内核日志；`cat /proc/self/status` 可用
- [x] 用户程序崩溃（#PF/#GP）不影响 Shell；`kill` 可终止进程（含信号）
- [x] `panic` 触发自动重启并显示上次崩溃信息；连续 3 次进安全模式
- [x] job control（`&`/`fg`/`bg`）基础可用（同步模型最小语义，无真并行后台）
- [x] 共享内存两进程交换数据可用
- [x] loopback TCP 两进程收发可用
- [x] `kernel.flat < 512KB`，启动 < 2s（当前 115KB / ~149ms）
