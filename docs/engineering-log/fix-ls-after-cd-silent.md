# fix-ls-after-cd-silent.md

## 结论：实际根因与修复（已验证）

**实际根因（非调度器）**：全局 fd 表（vfs.c `static file_t* fd_table[MAX_FDS]`）+ `vfs_fd_alloc`
从 0 起分配，导致**文件 open 抢占 std fd 0/1/2 槽位**。日志证据：ls 的 `open(".")` 与库加载
都拿到 `fd=1`（`[dbg] fstat fd=1 ino=225`）→ ls 的 stdout(fd 1) 被目录 fd 覆盖 → 列表写进
目录 fd → EISDIR → **ls 静默 exit code=2、无任何输出**。getdents64 本身工作正常（r=6376、EOF 正确）。

**修复（已实施并验证）**：
- `vfs_fd_alloc` / `vfs_fd_alloc_min` 从 fd 3 起分配（保留 0/1/2 给 stdio）。
- 新增 `vfs_fd_set`（精确槽位注册），sched_spawn_process 用它对空闲的 std 槽位逐个
  补注册 `/dev/tty`。
- 验证（test_ls_cwd 全绿，除因 cat 二进制污染日志导致的 /bin/pwd 脚本问题）：
  ls 列根目录 ✓、第二个 ls ✓、cd /usr/bin 后 ls 列出 vim ✓、cat 相对路径输出 ELF ✓。

**非内核问题的已知事项**：
- bash 的提示符/内建 echo/pwd 输出在 readline dumb 模式（无 terminfo）下延迟缓冲刷新，
  子进程退出时才刷出（历史已知限制，计划注入最小 terminfo）。
- `cat libtinfo.so.6` 报 ENOENT 是测试预期错误（该文件在 /lib/x86_64-linux-gnu/ 不在
  /usr/bin/）。

## Summary

用户建议在 `sched_mark_exited` 中加 `sched_wake_cond(WAIT_CHILD, ppid)` 唤醒父进程。
探索与证据表明**该诊断不成立**：

1. **唤醒机制完整且已被验证**：主循环在任务 ZOMBIE 时调用 `sched_wake_waiting_parent(t)`
   （kmain.c:344-346）→ 匹配 `p->pid==child->ppid && (want==-1 || want==child->pid)`
   （sched.c:391-403）→ `sched_wake(p)` 将父进程置 READY 并 **`mlfq_enqueue` 重新入队**
   （sched.c:288-297）。所有退出路径都经 `.Lreturn_to_kernel` 回主循环，ZOMBIE 分支必然执行。
2. **用户建议的调用是无效 no-op**：`sched_wake_cond` 要求 `wait_kind==kind && wait_arg==arg`
   （sched.c:328-329）；bash 在 `sys_wait4` 里 `sched_block_and_switch(WAIT_CHILD, want)`，
   wait_arg=**want**（子 pid 或 -1），**不是 ppid**。`sched_wake_cond(WAIT_CHILD, ppid)` 永不匹配。
3. **经验证明**：test_ls_cwd run#2 中第一个 `ls` 输出根目录后，bash 读取了 "cd /usr/bin"
   （日志 343 行回显发生在 bash 读 stdin 时）——bash 已从 wait4 恢复。调度器无问题。

**真正的剩余 bug**：`cd /usr/bin` 之后的第二个 `ls`（及后续 `cat`）加载、`open(".")`/
`open(libc.so.6)` 成功后，进程**静默无输出**（无列表、无错误消息、无异常转储）。内核主循环仍
存活（键盘回显继续），说明是用户态/系统调用路径上的静默失败或死循环，而非 bash 未被唤醒。

## Current State Analysis

已完成的修复（保留）：
- [table.c](file:///d:/BananaOS-Axion/src/kernel/syscall/table.c) `sys_fstat` 改用 `f->f_type`
  （EXT2_S_IFDIR=0x4000）→ `opendir` 的 `S_ISDIR(st_mode)` 检查通过 → 第一个 `ls` 正常列出根目录。
- `sys_open`/`sys_openat` 已支持相对路径/cwd/`.`/`..` 解析（`cwd_resolve`）。

当前观测（test_ls_cwd 两轮运行一致）：
- 第一个 `ls`（cwd=/）：完整工作（open(".") → fstat → getdents → 列表 → 退出 → bash 恢复）。
- 第二个 `ls`（cd 后）：ld.so 加载完成 → ls 主程序运行（open /proc/filesystems、/proc/mounts）
  → `open(".")` 成功（无 FAIL 打印）→ **之后零输出**。
- `cat`：bash fork/exec → ld.so 搜索 libc.so.6 → `open(/lib/x86_64-linux-gnu/libc.so.6)` 成功
  → **之后零输出**（无 "find library=libselinux"、无 "calling init"、无错误）。
- 全程无 IDT 异常转储（#GP/#PF 会打印并 hlt）。
- tty_read 回显（devfs.c:128）仅在进程读 stdin 时发生：日志 414/450 行 "cat"/"pwd" 回显
  说明 bash 当时在提示符处读取输入 → 说明第二 ls 与 cat **已静默退出**（或已回到 bash）。

两条最可能的假设（互斥，由 Change 2 仪器直接判定）：
- **H1**：`getdents64`/`ext2_getdents` 对目标目录立即返回 0 → ls 空列表 → exit(0)。
  可能成因：路径解析返回错误 inode（i_size=0 / 数据块为空）、或 getdents offset 处理异常。
- **H2**：`mmap` 后用户态 ld.so/程序在无系统调用的循环中卡死/静默退出
  （libc 被短读/垃圾填充 → ld.so 解析异常）。mmap 来自 `mmap_cursor` 全局游标，逐进程递增。

## Proposed Changes

### Change 1（已完成，保留）— sys_fstat 使用 f->f_type
保持 [table.c:1427-1440](file:///d:/BananaOS-Axion/src/kernel/syscall/table.c#L1427-L1440) 现状。

### Change 2 — 仪器化定位静默失败点（关键）
在 [table.c](file:///d:/BananaOS-Axion/src/kernel/syscall/table.c) 增加临时 `KERN_WARNING` 打印，
精确判定 H1 还是 H2：

| 位置 | 打印内容 |
|---|---|
| `sys_exit` | `[dbg] exit pid=%d code=%d`（判定进程是否静默退出及其状态码） |
| `sys_getdents64` | 入口 `[dbg] gd64 in=%u off=%llu cnt=%llu`；出口 `[dbg] gd64 r=%d next=%llu` |
| `ext2_getdents` | 入口 `[dbg] gd ino=%u size=%u nblk=%u`；每次 `ext2_read_block` 打印块号 |
| `sys_openat`/`sys_open` | 打印**解析后**路径（当前只打印原始 path，无法区分 "." 解析到 / 还是 /usr/bin） |
| `sys_fstat` | `[dbg] fstat fd=%d ino=%u ftype=%x mode=%llx` |
| `sys_mmap` | 入口 `[dbg] mmap addr=%llx len=%llu flags=%x fd=%d`；出口 `[dbg] mmap -> %llx` |
| `sys_read` | `[dbg] read fd=%d size=%u`；出口 `[dbg] read n=%d`（覆盖 loader 读 ELF 头与 mmap 文件读） |

（若 Change 2 打印过多导致串口竞争/重启——历史教训——则只保留 `sys_exit` + `sys_getdents64` +
`sys_openat` 解析路径三项。）

### Change 3 — 二分复现（缩小范围）
修改 [test_ls_cwd.ps1](file:///d:/BananaOS-Axion/test_ls_cwd.ps1) 或新建最小脚本，按序执行：
1. 裸 `ls`（应成功，基线）
2. 再裸 `ls` 一次（**无 cd**）→ 若第二个也静默，则 bug 是"第二次 exec"，与 cwd 无关
3. `cd /usr/bin` → `ls` → `cat libtinfo.so.6` → `pwd`
4. 每步之间用换行结尾的稳定标记（如 `echo` 输出或 bash 内建输出）确认 bash 回到提示符

观察日志末段最后一条 `[dbg]`，锁定卡死/静默退出发生在哪个系统调用。

### Change 4 — 按 Change 3 结论修复（三种对应方案）
- **F1（H1 成立：getdents 返回 0/空）**：核对 `ext2_lookup_path` 对目标目录返回的 inode 的
  `i_size`/数据块；若为错误 inode，修 `dcache`/`ext2_read_dir` 的匹配；`ext2_getdents` 增加
  `nblocks==0 || i_size==0` 时打印告警便于验证。修复方向：正确返回目录 inode。
- **F2（H2 成立：进程永不 exit，用户态循环）**：检查 `sys_mmap` 对库文件的读取完整性——
  `vfs_read` 短读时 `ext2_read_file` 因 `blk==0` break（稀疏/间接块解析错）导致映射页为垃圾 →
  ld.so 死循环。修复方向：`ext2_data_block`/inode 块表解析正确性，或 `sys_mmap` 对短读页补零
  与报错。
- **F3（H1/H2 之外：进程 exit 但输出丢失）**：检查 stdout/stderr 写入路径（`tty_write`）与该
  进程 fd 表状态（fd_table 为全局表，需确认 fork/exec/close 未破坏 fd 1/2）。

### Change 5 — 清理与回归
- 删除 Change 2 全部 `[dbg]` 打印（保留有价值的代码注释）。
- 删除临时脚本与 `ls_cwd_stream.log`。
- 回归 `test_w7_long.ps1`、`test_w7_fsimg.ps1`、修正后的 `test_ls_cwd.ps1`。

## Assumptions & Decisions

- **不修改 `sched_mark_exited`**：唤醒机制完整且已验证；用户建议的 `sched_wake_cond(WAIT_CHILD,
  ppid)` 是 no-op（匹配语义不成立）。若坚持加入，也只是无害空转，不解决问题。
- 保留 `sys_fstat` 的 `f_type` 修复（已由第一个 `ls` 正常列根目录验证）。
- fd 表是全局表（vfs.c:11），fork/exec 共享；Change 4-F3 才涉及该假设的核查。
- 优先使用最小仪器集，避免串口输出过多引发多核竞争（历史教训）。

## Verification

1. `test_ls_cwd.ps1` 全部通过（`ls` 列出根目录、`cd /usr/bin` 后 `ls` 列出 vim、`cat` 输出 ELF、
   `pwd` 返回 /usr/bin，且每一步后 bash 提示符恢复）。
2. `test_w7_long.ps1` 全绿（python3/vim 等长命令回归）。
3. `test_w7_fsimg.ps1` 全绿（绝对路径命令不受影响）。
4. 内核无新增 [dbg] 打印残留，kernel.flat < 80KB 约束保持（本改动为运行时 printk，不增 .bss/.rodata）。
