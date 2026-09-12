# 修复：`ls`（无参数）报 "cannot open directory '.'" 且不返回 bash 提示符

## Summary
用户诊断假设"ls 卡住 = sched_mark_exited 没有唤醒父进程"。探索证实该假设**不成立**：
- 唤醒机制完整且工作（kmain 主循环 ZOMBIE 分支 → `sched_wake_waiting_parent`；所有任务退出都经 `.Lreturn_to_kernel` 回到主循环，ZOMBIE 分支必然执行；long 测试实证 python3/vim exit 后 bash 正常恢复）。
- 用户建议的补丁 `sched_wake_cond(WAIT_CHILD, ppid)` **有匹配 bug**：`sched_wake_cond` 按 `wait_kind==kind && wait_arg==arg` 匹配，bash 的 `wait_arg` 是 `want`（-1 或子 pid），不是 ppid → 永不匹配，加了也不生效；正确版本是 `sched_wake_waiting_parent`（按 ppid + want 匹配），且现有机制已覆盖所有退出路径。

**确定根因（真实 bug）**：`sys_open` / `sys_openat` 不处理相对路径与 `.`/`..`，导致 `ls`（无参数）→ `opendir(".")` → `openat(AT_FDCWD, ".")` → `ext2_lookup_path(".")` 不认识 `.` → ENOENT → ls 打印 "cannot open directory '.'" 后 exit(1)。

## Current State Analysis（探索结论）
- **唤醒机制**：[kmain.c](file:///d:/BananaOS-Axion/src/kernel/kmain.c#L344-L347) 主循环在任务退出（`state==ZOMBIE`）后调用 `sched_wake_waiting_parent(t)`；[sched.c](file:///d:/BananaOS-Axion/src/kernel/sched/sched.c#L391-L403) 按 ppid + wait_arg 匹配唤醒父进程。[sched_mark_exited](file:///d:/BananaOS-Axion/src/kernel/sched/sched.c#L456-L465) 只标记 ZOMBIE，不负责唤醒（由主循环统一处理）。
- **执行模型**：[syscall_entry.S](file:///d:/BananaOS-Axion/src/kernel/syscall/syscall_entry.S#L195-L205) 的 `.Lreturn_to_kernel` 用全局 `user_ret_rsp` 恢复到主循环调用点 → ZOMBIE 分支总是执行。
- **exit_group 已实现**（table.c 231），exit 路径完整。
- **根因 bug**：
  - [sys_openat](file:///d:/BananaOS-Axion/src/kernel/syscall/table.c#L1856-L1861)：完全忽略 `dirfd`，直接 `sys_open(path, ...)`。
  - [sys_open](file:///d:/BananaOS-Axion/src/kernel/syscall/table.c#L51-L66)：直接 `vfs_open_mode(p, flags, mode)` 传原始路径，**不解析相对路径 / 不处理 `.` `..` / 不拼 cwd**。
  - 对比：[cwd_resolve](file:///d:/BananaOS-Axion/src/kernel/syscall/table.c#L380-L437) 已正确支持相对路径 + `.`/`..`（chdir/stat/rename 都在用），但 open 路径漏用。
  - 后果：`ls`（无参数，路径 `.`）、`cat 相对路径`、`cd` 后的任何相对路径命令全部 ENOENT。
- **为什么之前测试没暴露**：此前验证命令全部是绝对路径（`ls /bin/bash ...`、`cat /etc/passwd`）。

## Proposed Changes

### Change 1：`sys_open` 解析相对路径（根因修复）
**文件**：`src/kernel/syscall/table.c`（`sys_open`，约 51-66 行）
**做法**：
- 从用户态拷贝 path 到内核缓冲（复用 `exec_strncpy_from_user`，已有）。
- 若 `path[0] != '/'`（相对路径），用 `cwd_resolve(current_task->cwd[0] ? current_task->cwd : "/", pathbuf, resolved, sizeof(resolved))` 解析成绝对路径（正确处理 `.`/`..`/相对分量）。
- 用 `resolved` 调 `vfs_open_mode`。
- 绝对路径（含 `/dev`、`/proc`、`/sys`、`/bin/hello` 等）透传不变，不影响 devfs/tmpfs/procfs 分发。

### Change 2：`sys_openat` 支持 `dirfd`（glibc 默认路径）
**文件**：`src/kernel/syscall/table.c`（`sys_openat`，约 1856-1861 行）
**做法**：
- 拷贝 path。
- `dirfd == AT_FDCWD`（-100）或 path 为绝对路径 → 与 Change 1 相同处理（cwd_resolve 或透传）。
- 其他 dirfd（真实目录 fd）：用 `vfs_fd_get(dirfd)->name` 作基准目录，与 path 拼接（`cwd_resolve(base, path)`）。若 fd 无效或无 name → 返回 `-2`（ENOENT）/ `-9`（EBADF）。
- 定义 `AT_FDCWD` 为 `-100`。

### Change 3：验证 `ls` 卡住问题是否随根因修复消失
**做法**：修复后实测 `ls`（无参数）：
- 期望：列出根目录内容（`/` 下 bash 可见文件），正常退出，bash 返回提示符。
- 若仍卡住：抓运行日志确认卡点（退出路径已确认完整，理论上不会卡）。可疑残余点：`getdents64`（已确认实现正确，offset 单调推进）、tty 写入（错误消息很短，不会阻塞）。

## Assumptions & Decisions
- **不修改 `sched_mark_exited`**：唤醒机制完整（kmain 主循环 + `sched_wake_waiting_parent`），所有退出路径（正常 exit / exit_group / 异常杀进程）都回到主循环触发 ZOMBIE 分支。用户建议的 `sched_wake_cond(WAIT_CHILD, ppid)` 有匹配 bug（wait_arg ≠ ppid），且无条件在 exit 里唤醒有双重调度风险。
- `cwd_resolve` 对 `"."` 与空结果返回 `/`（根目录）——`opendir(".")` 在 cwd=`/` 时解析为 `/`，正确。
- 相对路径解析只发生在 `sys_open`/`sys_openat`；`vfs_open_mode` 内部（devfs/tmpfs/procfs 前缀分发）保持不变。
- 不扩展 `..` 的跨级检查（cwd_resolve 已弹栈到根为止，足够）。

## Verification
1. `powershell .\build.ps1` 构建成功（kernel.flat 链接、fs.img 生成）。
2. bash 下运行 `ls`（无参数）→ 输出根目录内容并返回提示符（不再报 "cannot open directory '.'"）。
3. `cd /usr/bin` 后 `ls` / `cat libc.so.6` 等相对路径命令正常。
4. 回归：`test_w7_long.ps1` 全绿（bash/python3/vim/exit）；`test_w7_fsimg.ps1` 中绝对路径命令不受影响。
5. 确认无卡死：每次外部命令退出后 bash 提示符正常恢复。
