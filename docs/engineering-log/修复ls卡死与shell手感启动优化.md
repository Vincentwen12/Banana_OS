# 修复 bash 执行 ls 后完全卡死 + shell 手感打磨 + 启动逻辑优化

## 摘要

用户实测：在 bash 提示符下输入 `ls`，目录列表正常输出，随后 bash **完全无响应**（输入任何命令无反应、无提示符）。readline 为正常模式（无 `turning off output flushing` 警告），TERM=xterm 已注入、terminfo 在盘，排除 dumb 模式刷新问题。需精确定位卡点并修复，同时打磨 shell 手感、优化启动逻辑。

## 现状分析（已核查，均正确）

- 退出路径完整：`sys_exit_group` → `sched_mark_exited`(ZOMBIE) → `syscall_exit_request=1` → [syscall_entry.S](file:///d:\BananaOS-Axion\src\kernel\syscall\syscall_entry.S) `.Lreturn_to_kernel` 用 `user_ret_*` 回主循环 → kmain ZOMBIE 分支 `sched_wake_waiting_parent` 唤醒 bash
- `fork_resume`/`sched_enter_user`/`switch_to_user` 均有 `SAVE_SCHED_RET`，`user_ret_rsp` 正确指向主循环调用点
- [sys_wait4](file:///d:\BananaOS-Axion\src\kernel\syscall\table.c#L289-L344)：保存/恢复全局 `syscall_user_rsp/ctx`，WAIT_CHILD 阻塞循环正确
- [sched_wake_waiting_parent](file:///d:\BananaOS-Axion\src\kernel\sched\sched.c#L391-L403) 按 ppid + wait_arg 匹配正确
- [ext2_getdents](file:///d:\BananaOS-Axion\src\kernel\fs\ext2.c#L1176-L1231) EOF 返回 0、offset 推进正确
- [sys_ioctl](file:///d:\BananaOS-Axion\src\kernel\syscall\table.c#L1296-L1353) TIOCGWINSZ/TCGETS 均即时返回不阻塞

**未确定**：ls 是否真正 exit；bash 的 wait4 是否被唤醒；bash 恢复后卡在哪一步（wait4 返回后 → 提示符 write → readline 等输入的 poll/read）。

## 拟议改动

### 步骤 1：加最小诊断日志定位卡点（一次性，定位后删除）

三个打印点，仅在实际事件发生时打一次，不刷屏：

1. [table.c](file:///d:\BananaOS-Axion\src\kernel\syscall\table.c) `sys_exit_group`/`sys_exit`：
   ```c
   printk(KERN_WARNING, "[dbg] exit pid=%d code=%d\n", (unsigned)(current_task?current_task->pid:0), (unsigned)code);
   ```
2. [table.c](file:///d:\BananaOS-Axion\src\kernel\syscall\table.c) `sys_wait4` 两处 return 前（reap 分支 + 循环 break 后）：
   ```c
   printk(KERN_WARNING, "[dbg] wait4 ret pid=%d\n", (unsigned)child->pid);
   ```
3. [table.c](file:///d:\BananaOS-Axion\src\kernel\syscall\table.c) `sys_getdents64` 返回 0 时：
   ```c
   if (r == 0) printk(KERN_WARNING, "[dbg] getdents EOF fd=%d\n", (int)fd);
   ```

用户跑一次 `ls`，据日志判定：
- 无 `exit` → ls 卡在用户态/其他 syscall
- 有 `exit` 无 `wait4 ret` → bash 未被唤醒（sched_find_child 的 `name==NULL` 过滤 或 sched_wake 匹配问题）
- 有 `wait4 ret` → bash 恢复后卡（提示符 write / 后续 readline poll 阻塞且键盘唤醒失效）

### 步骤 2：按诊断结果修复根因

候选项（按日志判定后选其一）：
- **bash 未被唤醒**：`sched_find_child` 中 `if (t->state == TASK_STATE_ZOMBIE && t->name == NULL) continue;` — execve 后 name 被改写为 NULL 时该 child 被跳过 → wait4 循环再次阻塞永不唤醒。修复：改为按 `t->pid == want` 或 ppid 直接匹配，去掉 name==NULL 条件。
- **bash 恢复后卡在等输入**：检查 tty 读阻塞（`tty_read` → WAIT_READ）与键盘唤醒（`sched_wake_kbd_all`）注册是否丢失；以及 readline 的 poll 路径 `poll_check_one` 对 tty 的 POLLIN 判定。
- **ls 未退出**：据 exit 日志缺失判断，进一步定位 ls 最后 syscall。

### 步骤 3：打磨 shell 手感

- 内核 shell [shell.c](file:///d:\BananaOS-Axion\src\kernel\core\shell.c)：命令历史上下键循环（当前仅支持解析/执行）
- 终端：确认退格/行编辑/回显体验（已有无条件回显）
- 移除测试脚本（test_pyrepl/test_shell_ux）中因 readline 完成列表 spam 引入的 `Start-Sleep 30` 等待（readline 已正常则无 spam）

### 步骤 4：优化启动逻辑

- 复核启动到 bash 提示符耗时（硬约束 <2s）：确认 readline 正常后无需 30s spam 等待，测试等待可缩短
- 清理启动期冗余输出（LD_DEBUG 已移除）

## 假设与决策

- 诊断日志为临时手段，根因定位后立即删除
- 不引入 terminfo/TERM 改动（已确认 readline 正常模式）
- 修复以最小改动为原则，不做超出根因的重构

## 验证

1. 用户手动：bash 下 `ls` → 正常回提示符 → 连续执行 `ls`/`echo`/`ps` 多条命令不卡死 → `exit` 正常退出
2. `test_shell_ux.ps1` 全绿（9/9，alive 项不再超时）
3. 诊断日志已删除，日志无 [dbg] 残留
