# W6 收尾计划：big 写入修复验收 + job control 警告消除 + 全量回归

## Summary

W6 生产级强化已进入收尾阶段。波 1（Task 2.4/2.5/3.2/3.3/3.4）源码已全部实现并构建通过；`test_w6_e2e.ps1` 实机验收显示 **perm/sig/shm/tcp 全部 PASS、bash echo/文件创建/cat/rm 全部正常**。仅剩两个真实缺口：

1. **Task 3.4**：`ext2_write_file` 原不支持两级间接块，导致 `test_big` 1MB 写入失败 —— **修复已由子代理落地**（`src/kernel/fs/ext2.c`），待构建验收。
2. **Task 3.1**：bash 启动打印 `initialize_job_control: no job control in background: Bad file descriptor` —— TIOCGPGRP 固定返回 0、setpgid 为空实现，job control 未真实化，**尚未实现**。

本计划完成剩余实现与全量验收，并勾选 tasks.md / checklist.md。

## Current State Analysis

### 已验证事实（来自 `w6_out.log` 实机输出）
- 构建通过：`kernel.flat` 111KB（<512KB），启动 ~136ms（<2s）。
- `run /bin/test_perm` → `perm-test PASS`；`test_sig` → `sig-handler-ok`；`test_shm` → `shm-test PASS`；`test_tcp` → `tcp-test PASS`。
- bash：`echo hello` 正常回显；`echo "persist" > /tmp/test.txt` → `cat` 输出 persist → `rm` 成功。
- `/proc/self/status`、`/sys/kernel/version`（Axion-Ban 0.5.0）、`ps` 均正常。
- 失败项：
  - `run /bin/test_big w` → `big-test write failed`（块号 ≥ 268 时 `ext2_write_file` 返回 -1）。
  - bash 启动第 60 行输出 `bash: initialize_job_control: no job control in background: Bad file descriptor`。

### 关键代码位置
- [ext2.c](file:///d:/BananaOS-Axion/src/kernel/fs/ext2.c#L369-L442) `ext2_write_file`：**已由子代理扩展两级间接块写路径**（`i_block[13]`：两级表 → 单级间接块表 → 数据块），读路径 `ext2_data_block`（L153-181）原本就支持两级，truncate 已支持释放。
- [table.c](file:///d:/BananaOS-Axion/src/kernel/syscall/table.c#L678-L728) `sys_ioctl`：`TIOCGPGRP(0x540F)` 固定写 pgrp=0；`TIOCSPGRP(0x5410)` 空 accept；`setpgid(109)` 在 [L550-553](file:///d:/BananaOS-Axion/src/kernel/syscall/table.c#L550-L553) 空实现。
- [devfs.c](file:///d:/BananaOS-Axion/src/kernel/fs/devfs.c)：`tty_c_lflag` 跟踪 c_lflag；`tty_read` 目前**无条件即时回显**（历史教训：readline dumb 模式清 ECHO 位，若按 ECHO 位回显打字不显示 —— **绝不可回退**）。
- [shell.c](file:///d:/BananaOS-Axion/src/kernel/core/shell.c)：命令注册表在 `shell_register()`（L85 附近），`cmd_run` 在 L469-520，用户进程同步执行（`user_run` 返回即进程退出）。
- `test_w6_e2e.ps1`：已存在，但 `Run-Test` 内 `$results +=` 是局部作用域，SUMMARY 只显示 3 项（漏了 6 项测试结果）——脚本 bug，需修。

### 约束（来自项目记忆）
- 内核二进制 <512KB（当前 111KB 余量充足）；启动 <2s。
- 用户进程同步执行模型：内核 Shell 的 `run`/bash 的 wait4 都是同步串行，**没有并行用户任务**。因此 `&` 后台/`fg`/`bg` 只能做最小语义（记录 + 同步运行 + `jobs` 列出），无法真后台。

## Proposed Changes

### Change 1（Task 3.1 核心）：job control 真实化，消除 bash 警告
**目标**：bash 启动不再打印 `no job control in background`。

bash 启动判定逻辑：`setpgid(0,0)`（把自己设为组长 pgrp=pid）→ `tcsetpgrp(stdin, pgrp)` → `tcgetpgrp(stdin)` 与自身 pgrp 比较，不一致即报 "no job control in background"。当前 TIOCGPGRP 永远返回 0 → 必然不一致。

- `src/kernel/fs/devfs.c`：新增 per-tty 前台进程组变量（如 `static uint32_t tty_fg_pgrp`），提供 `devfs_tty_get_pgrp()/devfs_tty_set_pgrp()` 接口。
- `src/kernel/syscall/table.c` `sys_ioctl`：
  - `TIOCGPGRP`：返回 `tty_fg_pgrp`（而非固定 0）。
  - `TIOCSPGRP`：读用户传入 pgrp（`vm_copy_from_user` 4 字节）→ `devfs_tty_set_pgrp()`。
- `src/kernel/syscall/table.c` `sys_setpgid(pid, pgid)`：`current_task` 增加 `pgrp` 字段（`sched.h` 的 `task_t`），setpgid 时 `pgid==0` 取 `current_task->pid`；提供/接入 `getpgrp`（syscall 111 附近已有占位则改为返回 `current_task->pgrp`）。
- **验收**：bash 启动无该警告；`echo hello` 仍正常。

### Change 2（Task 3.1 附加）：内核 Shell job control 最小命令
同步模型下做最小语义：
- `src/kernel/core/shell.c`：
  - `cmd_run` 解析尾随 `&`（argv 最后一项以 `&` 结尾则剥离并标记 background），打印 `Started job N (pid P) in background`。
  - 新增 `jobs` 命令：维护一个静态 job 表（pid/路径/状态，上限 16），`jobs` 列出所有条目；任务同步跑完后状态置 `Done`。
  - `fg <pid>`：在 job 表找到未完成任务则重新同步运行该路径（最小语义：重新 `run`）；`bg <pid>`：仅置状态 `Running`（告知同步模型下无真后台）。
  - 通过 `shell_register()` 注册（**不要用局部数组初始化 cmd_table**，历史教训）。
- `tools/sleep.S`（新增）：busy-wait 约 2 秒后 `exit(0)` 的小 ELF（参考 `test_sig.S` 的构建方式）；`build.ps1` 的 fs.img 注入列表注册为 `/bin/sleep`。
- **验收**：`run /bin/sleep 2 &` 后 Shell 立即可用；`jobs` 能列出该任务；`fg` 可收回前台同步执行。

### Change 3：修复 `test_w6_e2e.ps1` 结果汇总 bug
- `Run-Test` 函数内 `$results += ...` 改为函数返回 PASS/FAIL 字符串，外层收集；或把 `$results` 声明为脚本级并在函数内用 `$script:results` 引用。
- 同步修复 `big_write` 断言（`big-test WROTE`）与 `big_verify`（`big-test PASS`）等待时间；给 `big_verify` 后追加一次 `cat /tmp/big.bin` 无关紧要，保持最小改动。

### Change 4：勾选 tasks.md / checklist.md
- `.trae/specs/w6-production-hardening/tasks.md`：勾选 Task 2.4/2.5/3.1/3.2/3.3/3.4 与完成标志中已实测通过项。
- `.trae/specs/w6-production-hardening/checklist.md`：逐项勾选，未过项标注原因（如真后台并行受同步模型限制）。

## Assumptions & Decisions

- **不询问用户，直接按最小可行实现推进**（用户要求快、省 API）。Task 3.1 的 `&`/`fg`/`bg` 采用同步模型下的最小语义，如实注释"无真并行后台"。
- **不改 `tty_read` 回显行为**（历史教训，避免打字不显示的回归）。
- **不改 ext2.c 已落地的两级间接写**，仅构建验收；若验收失败再派子代理修复。
- 不加新特性、不做超范围重构。

## Verification

1. `powershell -ExecutionPolicy Bypass -File .\build.ps1` 构建通过，`kernel.flat < 512KB`。
2. `powershell -ExecutionPolicy Bypass -File .\test_w6_e2e.ps1`：
   - `perm-test PASS`、`sig-handler-ok`、`shm-test PASS`、`tcp-test PASS` 保持。
   - `big-test WROTE checksum=...` + `big-test PASS`（两级间接写 1MB 成功）。
   - bash 启动无 `no job control` 警告；`echo hello` 正常。
   - `cat /proc/self/status`、`cat /sys/kernel/version`、`ps` 正常。
3. 交互验证 job control：`run /bin/sleep 2 &` → `jobs` 列出 → `fg` 同步运行。
4. 勾选 tasks.md / checklist.md 对应项。

## Task 分工建议

- 主代理：Change 1+2 实现（或派 1 个子代理 E 实现 3.1，与主代理写验收脚本并行）；Change 3 脚本修复；Change 4 勾选。
- 构建与验收统一由主代理在代码改动完成后串行执行（避免子代理并发构建的工件冲突）。
