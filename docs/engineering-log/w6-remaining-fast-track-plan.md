# W6 剩余任务「快速高质量」执行计划

> 目标：在保证质量（每任务内联验收、回归不返工）的前提下，通过**并行子代理 + 严格文件归属 + 串行构建验证**把剩余 Task 2.4 / 2.5 / 3.1 / 3.2 / 3.3 / 3.4 与最终回归一次跑完。

## 一、Summary

按已批准 `w6-production-hardening` Spec，剩余 6 个任务分 3 波执行：

| 波次 | 任务 | 并行子代理 | 内容 |
|---|---|---|---|
| 波 1 | 2.4 + 2.5、3.2、3.3、3.4 | 4 个（上限） | 多用户+信号 / 共享内存 / loopback / 磁盘写优化 |
| 波 2 | 3.1 | 1 个 | termios 完整 + job control（依赖 2.5） |
| 波 3 | 最终回归 | 主代理 | 全量回归 + checklist/tasks 勾选 + 完成标志 |

加速手段：① 波 1 四路并行；② 每个任务**内联验收**（波内构建一次，波后逐项验证），不做集中式大回归；③ 提前在 `build.ps1` 注册全部新源文件占位，保证并行期构建不因缺文件失败；④ 各子代理只改**自己专属文件**，避免并发编辑冲突；⑤ 主代理在波 2 并行期间独立编写端到端回归脚本。

质量保障：每个子代理的 prompt 内置工程约定（AT&T 语法、bananaos.h、-O2 无栈数组 cmd_table、objdump 技巧、构建命令显式化），且每个任务都有明确验收命令。

## 二、当前状态分析（已探明）

- `kernel.flat` 现 103,068 B（< 503KB 硬限，余量充足，可安全扩展 task 结构）。
- [table.c](file:///d:/BananaOS-Axion/src/kernel/syscall/table.c)：`rt_sigaction`(13)/`rt_sigprocmask`(14)/`rt_sigreturn`(15) 均为空 stub；`kill`(62) 仅支持 SIGKILL=9；`getuid/getgid/geteuid/getegid`(102/104/107/108) 恒返 0；ioctl(16) 的 TCGETS/TCSETS 已读写 `tty_c_lflag`，TIOCGPGRP(0x540F) 恒返 0、TIOCSPGRP(0x5410) 空接受。
- [sched.h](file:///d:/BananaOS-Axion/src/kernel/sched/sched.h) task_t：已有 pid/ppid/exit_code/fork_ctx/mm_context/kstack_top，**无 uid/gid/pgid/信号字段**。
- [devfs.c](file:///d:/BananaOS-Axion/src/kernel/fs/devfs.c)：tty 为单字符阻塞读 + **无条件回显**（dumb readline 权衡），无 ICANON 行缓冲/VMIN/VTIME/ISIG。
- [syscall_entry.S](file:///d:/BananaOS-Axion/src/kernel/syscall/syscall_entry.S)：syscall 返回处先查 `syscall_exit_request`/`syscall_exec_request`，否则弹用户帧 `sysretq` —— 信号投递注入点即在此处（仿照 exit/exec 的全局标志机制）。
- [idt.c](file:///d:/BananaOS-Axion/src/kernel/core/idt.c) `exc_kill_user(vec)`：当前一律映射信号并杀进程，未查 handler 表。
- ext2 inode（128B 标准结构）已在 ext2.h 定义：`i_mode`@0、`i_uid`@2、`i_gid`@24；`ext2_create_file(parent_ino, name, mode)` 已接受 mode。
- 构建：`powershell -ExecutionPolicy Bypass -File build.ps1`（生成 bootsect/kernel/fs.img/disk.img，末尾会起 QEMU nographic）。
- 回归运行模式：`test_regression.ps1` 用 `ProcessStartInfo` 起 QEMU（`-drive format=raw,file=disk.img -drive format=raw,file=fs.img -m 2G -nographic -no-shutdown -smp 4 -machine pc,accel=tcg`），靠 `BeginOutputReadLine` 收 `\n` 结尾的标记行（如 `initialize_job_control`）判就绪。

## 三、波 0（主代理，执行开始立即做，10 分钟内）

1. **注册新源文件占位**：在 `build.ps1` 的编译列表追加空文件 `src/kernel/syscall/signal.c`、`src/kernel/syscall/shm.c`、`src/kernel/net/loopback.c`（空文件可编译），并创建这三个空 .c/.h 骨架 + `src/kernel/net/` 目录。→ 保证并行期构建不因缺文件失败。
2. **预留 table.c 锚点**：在 table.c 末尾追加两个唯一注释锚：
   - `/* [ANCHOR-SHM] append shm syscall registrations below */`
   - `/* [ANCHOR-NET] append net syscall registrations below */`
   波 1 的 B/C 子代理各自以**本锚点整行**为 SearchReplace old_str 的锚，互不重叠、不会互相吞掉。
3. 记下当前 git 状态（不提交），作为波后验收基线。

## 四、波 1（4 个并行子代理，只改代码不构建）

> 并发限制：最多 4 个子代理。**各自禁跑 build.ps1/QEMU**（避免磁盘镜像竞争），由主代理在波 1 全部返回后串行构建验证。

### 子代理 A：Task 2.4 多用户基础 + Task 2.5 信号完整性（文件归属：sched.h/sched.c、table.c 的既有行、signal.c 新文件、idt.c、syscall_entry.S、测试 ELF）

**A1. uid/gid（Task 2.4）**
- sched.h：task_t 增加 `uint32_t uid, gid;`（默认 0）。
- table.c：`sys_getuid/getgid/geteuid/getegid` 改为返回 `current_task->uid/gid`；新增 `sys_setuid(105)/sys_setgid(106)` 写 current_task->uid/gid（供测试模拟非 root）。
- 文件权限检查：**hook 放 vfs.c**（A 独占），在 vfs_open 的 ext2 分发路径后调用 `perm_check_ext2(file_t*)`：从 `file->private_data`（预期为 ext2 inode 指针，若实际不是则探明后改从 ext2.h 提供的只读 API 取）读 i_mode/i_uid/i_gid；`uid==0` → 放行（root 语义）；uid 匹配用 owner 位、gid 匹配用 group 位、否则 other 位；读（O_RDONLY/O_RDWR）需 r 位、写（O_WRONLY/O_RDWR/O_TRUNC/O_CREAT）需 w 位；拒绝返回 `-13` (EACCES)。procfs/sysfs/tmpfs/devfs 不做权限检查。
- `/etc/passwd` 解析（最小实现）：kmain 或 vfs_init 后调 `users_init()`：从 ext2 只读打开 `/etc/passwd`，解析 `root:x:UID:GID:...` 首行；文件不存在则 uid=0 兜底。同时把 `sys_open` 的 mode 参数（a4）透传给 ext2_create_file 的 i_mode（供 O_CREAT mode=0 造无权限文件）。
- 测试 ELF：`tools/test_perm.S`（as+ld 方式，仿 test_fault.S）：open("/tmp/secret.txt", O_WRONLY|O_CREAT|O_TRUNC, 0) → 写内容 → close → setuid(1000) → open(O_RDONLY) 预期返回 EACCES → 输出 `perm-test PASS/FAIL`。构建安装机制沿用 test_fault 同款（build.ps1/gen_tools 中注册 `/bin/test_perm`）。
- 验收：bash 内 `run /bin/test_perm` → PASS；`getuid` 语义对 bash 无回归。

**A2. 信号（Task 2.5）**
- sched.h：task_t 增加 `uint64_t sig_handler[64]; uint64_t sig_pending, sig_blocked;`（每任务 528B，BSS 增量按 MAX_TASKS 核验，余量充足）。
- 新文件 `syscall/signal.c`：
  - `sys_rt_sigaction(13)`：从用户态拷贝 sigaction（offset0=handler、8=flags、16=restorer）；存 `sig_handler[sig]`；SIGKILL/SIGSTOP 忽略；oldact 回写。
  - `sys_rt_sigreturn(15)`：从用户栈 sigframe 恢复 syscall_user_ctx 全寄存器 + syscall_user_rsp，清除投递态，正常 sysretq 续跑。
  - `sys_kill(62)` 扩展：目标有 handler 且未 blocked → 置 pending 位 + 若在等待则唤醒；无 handler → 默认动作：SIGKILL/终止类 → sched_kill/退出，SIGCHLD → 忽略。支持 `pid<0`（进程组）最小语义。
  - `sys_rt_sigprocmask(14)`：设置/清除 blocked 位。
  - `signal_setup_handler(sig)`：计算新用户 RSP = syscall_user_rsp − sizeof(sigframe)；把保存的寄存器快照 + 旧 RIP/RSP 写入用户栈 sigframe；sigframe 返回地址取 SA_RESTORER(0x04000000) 时的 restorer，否则取内核信号恢复桩地址；设全局 `syscall_sig_handler/syscall_sig_frame_rsp/syscall_sig_request=1`。
  - `sched_kill_group(pgid, sig)`：遍历 task_table 按 pgid 投递（供 3.1 ISIG 复用，先实现）。
- `syscall_entry.S`：`call syscall_dispatch` 后、弹用户帧前，`cmpl $0, syscall_sig_request(%rip); jne .Ldeliver_sig`；`.Ldeliver_sig` 清标志后改为 `movq syscall_sig_frame_rsp(%rip),%rsp`（弹 sigframe 栈）+ `movq syscall_sig_handler(%rip),%rcx`（RIP=handler）再 `sysretq`。
- **信号恢复桩**：kmain（或 signal_init）把一页物理页映射到**每个进程**用户地址 `0x7FFFFFF00000`（VM_USER|RW|X，在 vm_create/sched_spawn_process 处映射），内容 `mov $15,%eax; syscall; ud2`（rt_sigreturn 桩）。
- `idt.c` `exc_kill_user`：先查 `sig_handler[映射信号]`，有 handler → 置 pending（走投递而非杀进程）；无 → 维持现状杀进程。
- 测试 ELF：`tools/test_sig.S`（`/bin/test_sig`）：① 注册 SIGTERM handler → `kill(getpid(),15)` → handler 执行打印 `sig-handler-ok` → 正常退出；② 不注册场景由 bash `kill` 验证默认终止。
- 验收：bash 内 `run /bin/test_sig` 输出 handler 标记；bash 中 `kill <pid>` 可终止进程；#PF 无 handler 仍杀进程（回归 test_fault）。

### 子代理 B：Task 3.2 共享内存最小实现（文件归属：shm.c 新文件 + table.c 锚点 SHM + 测试 ELF）

- 新文件 `syscall/shm.c`：SysV 编号 29 shmget / 30 shmat / 31 shmctl / 67 shmdt（**先核对 table.c 这 4 个编号当前未注册**，被占则改用 Linux 替代编号并在测试 ELF 中对齐）。8 槽位表（key/size/phys/ref）；`shmget` 从 Ω pmalloc 分配页（4KB 对齐取整）；`shmat` 用 `vm_map_page(mm_context, 0x6000000000+slot*4KB, phys, VM_USER|RW)` 映射到当前任务；`shmdt` 解除；`shmctl(IPC_RMID)` 释放；简单自旋锁保护槽表。
- table.c：在 `[ANCHOR-SHM]` 锚下追加 4 个注册。
- 测试 ELF：`tools/test_shm.S`（`/bin/test_shm`）：父进程 shmget→shmat→写 "hello-shm"→fork→子进程 shmget 同 key→shmat→读→校验→打印 `shm-test PASS`→exit→父 wait4。单进程单 fork，不依赖 job control。
- 验收：bash 内 `run /bin/test_shm` → PASS。

### 子代理 C：Task 3.3 loopback TCP（文件归属：net/loopback.c 新文件 + table.c 锚点 NET + 测试 ELF）

- 新文件 `net/loopback.c`：16 端口表；每监听端口挂待接受队列；每条连接两个环形缓冲（对端写→本端读）；阻塞用现有 `sched_block/sched_wake` 机制（探明 sched.c 阻塞 API；若无门铃通道可阻塞，则用 sched_yield 轮询 + 标志位，注释说明）。地址仅接受 127.0.0.1。
- syscall 注册（Linux x86_64 编号）：41 socket / 42 connect / 43 accept / 44 sendto / 45 recvfrom / 49 bind / 50 listen / 48 shutdown（socket 返回 vfs fd：用 file_ops + vfs_fd_alloc，读写走 read/write 或专用 syscall）。阻塞型 accept/recv 用每连接 wait 标志 + sched_wake。
- table.c：在 `[ANCHOR-NET]` 锚下追加注册。
- 测试 ELF：`tools/test_tcp.S`（`/bin/test_tcp`）：父=server（bind 127.0.0.1:7777→listen→accept→recv "ping"→send "pong"），fork 子=client（connect→send "ping"→recv "pong"→打印 `tcp-test PASS`）→父 wait4。
- 验收：bash 内 `run /bin/test_tcp` → PASS。

### 子代理 D：Task 3.4 磁盘写入优化（文件归属：ext2.c/ext2.h 独占 + 测试 ELF）

- 审计 `ext2_write_file` 的多块路径：确保 >12 直块的续写正确分配单间接块、`i_size/i_blocks` 与目录项同步回写（防 ≥1MB 丢失的核心）。
- 性能优化（可选，不改变语义）：若 ATA PIO 每扇区握手开销大，做 4KB 级批量写（RAM 内拼 8 扇区后连续发命令）；实现简单则做，复杂则只保证正确性（验收只要求无丢失）。
- 测试 ELF：`tools/test_big.S`（`/bin/test_big`，带参数）：`test_big w` 以 1KB 分块连续 write 1MB 到 `/tmp/big.bin`（覆盖 + 校验和内存累计）；`test_big v` 重开文件重读 1MB 校验和，`校验和一致` → `big-test PASS`。写盘用现有 ext2 写路径（跑完关 fs.img 前 flush）。
- 验收：bash 内 `run /bin/test_big w` → 重启（自动或手动）→ `run /bin/test_big v` → PASS。

### 波 1 收尾（主代理）
- 依次构建（串行）：`powershell -ExecutionPolicy Bypass -File build.ps1`；若编译错，按文件归属定位到对应子代理修复。
- 逐项验收：test_perm / test_sig / test_shm / test_tcp / test_big(w) 在 QEMU 中跑，随后重启验证 test_big(v) 持久化。
- 更新 tasks.md（勾选 2.4/2.5/3.2/3.3/3.4）与 checklist.md。

## 五、波 2（1 个并行子代理 + 主代理并行写回归脚本）

### 子代理 E：Task 3.1 termios 完整 + job control 基础（文件归属：devfs.c、table.c ioctl 区、sched.h pgid、新 sleep 工具；依赖波 1 的 2.5 信号与 kill-group）

- devfs.c：
  - ICANON 行缓冲：tty_read 按 `c_lflag&ICANON` 决定逐字符返回还是攒到 `\n` 返回整行；`tty_cc[VMIN/VTIME]`（cc 数组），VMIN=0/VTIME 超时用 timer 轮询 kbd。
  - ECHO 按位回显（`c_lflag&ECHO`），ECHONL 处理 `\n`；**回归风险**：dumb readline 可能再次出现"打字不显示"（历史教训），若波 3 回归检测到，回退为"ICANON off 时无条件回显"并注释权衡。
  - ISIG：Ctrl-C(0x03)→SIGINT、Ctrl-Z(0x1A)→SIGTSTP、Ctrl-\ (0x1C)→SIGQUIT，投递到前台进程组（`sched_kill_group(tty_fg_pgid, sig)`），不进入缓冲区。
- table.c ioctl：TCGETS/TCSETS 扩展读写 cc 数组与更多 c_lflag 位；TIOCGPGRP(0x540F) 返回 `tty_fg_pgid`；TIOCSPGRP(0x5410) 设置；TIOCSCTTY(0x540E) 接受（置控制终端）。
- sched.h/sched.c：task_t 增加 `pgid`；实现 `sys_setpgid(149)/sys_getpgrp(111)/sys_setsid(112)`（setsid：pgid=pid）。
- 工具：在 gen_tools.py 或 as+ld 机制新增 `/bin/sleep`（`sleep N` 循环 + `exit`）。
- 验收：bash 启动无 "no job control" 警告；`sleep 2 &` → `jobs` 显示 [1]+ Running；`fg` 收回前台；前台跑 `cat` 时 Ctrl-C 杀之；Ctrl-C 不再把整个 bash 干掉。
- **主代理并行**：编写 `tools/test_w6_e2e.ps1`（仿 test_regression.ps1 的 ProcessStartInfo 模式，仅 ASCII 注释）：依次跑 hello/echo>文件/cat/rm/ps/dmesg/test_perm/test_sig/test_shm/test_tcp/test_big w/jobs/fg/持久化校验，全部 PASS 才算结束。

## 六、波 3（主代理）：最终回归与收尾

1. 跑 `test_w6_e2e.ps1` 全量回归（含 0.1-2.3 既有项）；修剩余问题（按文件归属定位）。
2. 跑 `test_big v`（重启后）验证 1MB 持久化；确认 `kernel.flat < 512KB`、启动 < 2s。
3. 勾选 tasks.md 全部剩余项与 checklist.md 完成标志。
4. 不再创建额外文档（用户未要求）；最终给出口头总结。

## 七、假设与决策

- **并行安全**：子代理只改专属文件；table.c 通过预置 `[ANCHOR-SHM]/[ANCHOR-NET]` 唯一锚点隔离追加；sched.h 波 1 归 A、波 2 归 E，串行不冲突。
- **构建串行**：所有子代理不运行构建/启动命令，由主代理串行 build.ps1，避免 disk.img/fs.img 竞争。
- **权限检查 hook 在 vfs.c**：从 file->private_data 读 ext2 inode 元数据，**不改 ext2.c**（与 D 隔离）；若 private_data 非 inode 指针，A 探明后改用 ext2.h 只读字段或最小 getter（getter 若必须写 ext2.c，则改由 D 代为添加或 A 在 vfs.c 内联读取，两难时以"不改 ext2.c"为第一原则）。
- **shm/网络测试用 fork 单程序**（不依赖 job control），test_big 分写/验两个模式跨重启校验。
- **信号恢复桩**固定映射 0x7FFFFFF00000 到每个用户 mm（在 vm_create 路径加），SA_RESTORER 优先。
- **回显权衡**：ECHO 按位实现若回归 dumb readline，回退"ICANON off 无条件回显"。
- **syscall 编号**：shm/socket 系列先核对 table.c 空闲再注册，测试 ELF 与内核严格一致。
- kernel.flat 余量 ~400KB，task 结构扩展（uid/gid/pgid + 信号 528B/task + socket 表）无体积风险。

## 八、验证清单（对应 checklist）

- 2.4：`run /bin/test_perm` PASS；getuid 返回真实值
- 2.5：`run /bin/test_sig` handler 执行；kill 默认终止；#PF 无 handler 杀进程（test_fault 回归）
- 3.2：`run /bin/test_shm` 两进程（父子）交换数据 PASS
- 3.3：`run /bin/test_tcp` 双向收发 PASS
- 3.4：`test_big w` → 重启 → `test_big v` 校验和一致
- 3.1：bash 无 "no job control" 警告；`sleep 2 &`+`jobs`+`fg`；Ctrl-C 只杀前台
- 最终：`test_w6_e2e.ps1` 全 PASS；`kernel.flat < 512KB`；启动 < 2s
