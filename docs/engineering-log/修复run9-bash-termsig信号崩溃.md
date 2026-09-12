# 修复 run9 bash termsig_sighandler 崩溃（信号投递路径）

## 摘要
iso_s_imports.ps1 跑重 import 时，bash 连续 8 次 fork/exec python 后在第 9 轮崩溃，随后进入崩溃循环：每次重启的 bash 都以相同签名崩溃
`user fault vec=14 sig=11 pid=4 rip=0x2008b410(=bash termsig_sighandler) cr2=0x10206`。
即 bash 收到某个终止类信号 → 进入 termsig_sighandler → 处理器入口即 #PF（cr2 为极小地址）→ 无 SIGSEGV 处理器 → 被杀 → 登录循环重启 → 再次收到信号再崩。
现象指向内核信号投递路径或 task 槽状态累积，与 brk/mmap 无关（修复 brk/mmap 后此崩溃不变，2 次试验均稳定 run9 崩溃）。本计划先加最小插桩精确定位信号来源与槽状态，再按其定因修复并回归。

## 当前状态分析（代码勘查结论）

### 崩溃链路（已确认）
- 日志来自 `src/kernel/core/idt.c` `exc_kill_user()`（L96）：`user fault vec=%u sig=%u pid=%u rip=%x cr2=%x`。
- bash 崩溃时 rip=0x2008b410 = PIE bash 偏移 0x8b410 的 `termsig_sighandler`（objdump 已确认）。termsig 处理 SIGINT/SIGHUP/SIGQUIT/SIGTERM/SIGALRM 等终止类信号。
- bash 无 SIGSEGV 处理器 → exc_kill_user 走 kill 分支 → `sched_mark_exited` + `syscall_exit_request` → 登录循环 `sched_force_reap` + `shell_launch_program`（`sched_spawn_process`）重启 bash → 再次以相同签名崩溃（崩溃循环）。

### 信号投递机制（已通读 signal.c + syscall_entry.S）
- `syscall` 返回路径：dispatch 后 call `signal_check_deliver()` → 若有 `sig_pending & ~sig_blocked` 且 `sig_handler[sig] > SIG_IGN` → `signal_setup_delivery()` 在 `(syscall_user_rsp - 144) & ~0xF` 写 sigframe 并置全局 `syscall_sig_request/handler/frame_rsp` → asm sysretq 到 handler，RSP=frame 起始。
- 异常路径：`exc_kill_user()` → `signal_deliver_now()` 走同一套全局。
- 恢复：handler 经 restorer（libc SA_RESTORER 或内核桩）调 rt_sigreturn → 置 `syscall_sig_restore_request/rip/rflags` → asm 恢复。

### 疑点（候选根因，按代码证据排序）
1. **跨任务信号全局残留**：`syscall_sig_request/handler/frame_rsp` 及 `syscall_sig_restore_*` 是**全局**，属"当前任务"。bash 在 `sys_wait4` 期间子进程（python）的系统调用若在退出前臂置了这些全局（子进程退出走 `.Lreturn_to_kernel`，不清 `sig_request`），父 bash 唤醒 sysretq 回用户态时会命中 asm 检查（`exit→exec→sig_restore→sig_request`）用**子进程的 handler/frame** 投递。子进程 pre-exec（fork 恢复期）继承 bash 的 sig_handler（termsig 等），frame 地址在 bash mm 中内容为任意旧数据 → bash 进 termsig、寄存器来自垃圾帧 → 极小地址 #PF。
   - 佐证：`sys_wait4`（table.c）只保存/恢复 `syscall_user_rsp/syscall_user_ctx`，**未含 sig 请求全局**（历史修复记录针对 RSP/ctx，非 sig 全局）。
2. **死亡任务定时器残留**：`sched_poll_timeouts()`（sched.c L356）对 task_table 全部任务（含 ZOMBIE/REAPED）检查 `itimer_deadline` 并投递 SIGALRM，且周期型会**无限重排**；`sched_mark_exited()`/`sched_kill()`/`sched_force_reap()` 都不清 itimer → 幽灵 SIGALRM 风暴打到已死/待复用槽。`sched_spawn_process` 复用槽时虽重置 itimer/pending，但死亡窗口内的投递可能打到错误的当前任务。
3. **槽位复用残留**：`sched_spawn_process()` 未重置 `fs_base/gs_base`（fork 会复制它们）；`sys_execve()` 也未清 fs/gs（Linux exec 应清）。新 bash 在 glibc 建 TLS 前若被信号中断，termsig 的 `mov %fs:0x28` 栈金丝雀读取会用陈旧 fs_base → 极小地址 #PF（cr2=0x10206 ≈ fs_base+0x28 形态吻合）。
4. 信号具体来源不明：需要区分 SIGALRM（ITIMER 轮询）、bash 自 kill、或 sys_kill 误投。

### 实测基线（供回归对比）
- 最终 mmap/munmap 修复版：run1-8 OK，run9 crash（2/2）。
- HEAD 基线：bash 启动即崩（另一个问题，已修复）。

## 建议改动

### Phase 1 — 最小插桩（先取证，改动小、带标识可 grep）
1. `src/kernel/syscall/signal.c`：
   - `kill_one_task()` 入口打印：`[sigdbg] kill cur=%u->tgt=%u sig=%u h=%x pending-old=%x`（仅 sig != SIGCHLD，避免刷屏）。
   - `signal_check_deliver()` 臂置成功后打印：`[sigdbg] deliver pid=%u sig=%u h=%x fr=%x ctxrip=%x`。
   - `signal_deliver_now()` 打印同款（异常路径）。
2. `src/kernel/sched/sched.c` `sched_poll_timeouts()`：ITIMER 触发时打印 `[sigdbg] itimer tid=%u pid=%u iv=%u st=%u`。
3. `src/kernel/core/idt.c` `exc_kill_user()`：现有 crash 行后追加一行全量现场：
   `[crash] rsp=%x pend=%x blk=%x h2=%x h14=%x h15=%x sigreq=%d siginner=%x restore=%d fs=%x`
   （rsp 用传入 fault_rsp；h* 取 current_task->sig_handler[2/14/15]；sigreq/siginner/restore 读 signal.c 全局 `syscall_sig_request/syscall_sig_handler/syscall_sig_restore_request`，fs 取 current_task->fs_base——需先确认这些符号在 idt.c 可见：signal.c 全局变量为非 static，加 extern 声明即可）。
   → 这一行能一次性判定：是"真实信号 + handler 帧错误"还是"残留全局误投"或"fs 金丝雀错位"。

### Phase 2 — 按取证结果修复（决策表）
运行 iso_s_imports.ps1 复现，读 `[sigdbg]/[crash]` 行，按下表执行：
| 取证信号 | 定因 | 修复 |
|---|---|---|
| `sigreq=1` 且 `siginner/handler` 为子进程遗留（与当前 pid 不符） | 跨任务信号全局残留（疑点 1） | 修复 F2：`sys_wait4` 在 child_runner 前后保存/恢复 `syscall_sig_request/sig_handler/sig_frame_rsp/sig_frame_ptr/sig_restore_request/restore_rip/restore_rflags`；`.Lreturn_to_kernel`（syscall_entry.S）清零全部 sig 请求全局（`movl $0` x3 + 关联字段），并在 `sys_execve`/`sched_mark_exited` 处置 `syscall_sig_request=0` |
| `[sigdbg] itimer` 命中已 ZOMBIE/REAPED 或非预期任务 | 死亡任务定时器残留（疑点 2） | 修复 F1：`sched_mark_exited()` 与 `sched_kill()` 清 `itimer_deadline=itimer_interval=0`；`sched_poll_timeouts()` 跳过 ZOMBIE/REAPED（直接 continue，不重排） |
| `[crash] fs=0x10xxx`（fs 基址为极小值）+ 崩溃点在函数金丝雀序言 | fs/gs 复用残留（疑点 3） | 修复 F3：`sched_spawn_process()` 置 `fs_base=gs_base=0`；`sys_execve()` 清 `current_task->fs_base/gs_base`（Linux exec 语义）并同步 MSR 由 `syscall_restore_msrs` 按任务字段恢复 |
| 三者皆否，`[crash] sigreq=0` 且 fs 正常 | 真实信号在错误时机投递 | 修复 F4：`signal_check_deliver()`/`signal_deliver_now()` 增加护栏——`handler < 0x10000` 或 `frame_rsp` 对应页不可写（`vm_range_present` 校验 144B 区间全映射，复用 vm.h 现有函数）时放弃投递（不置 sig_request），避免 sysretq 到坏帧 |

### Phase 3 — 回归验证
1. 重建：`powershell -ExecutionPolicy Bypass -File build.ps1 -SkipFs`。
2. `.\iso_s_imports.ps1`：期望 10/10 OK 或至少较 run9 崩溃明显后移；记录最终崩溃点（如有）签名与 `[crash]` 行。
3. 回归（信号改动影响面）：`.\diag_py_imports.ps1`（M0-M19 探针）与 `.\test_bash_restart.ps1`（bash 登录重启）确认不引入新回归。
4. 收尾：删除插桩打印（或保留为 `[sigdbg]` 并用条件编译/console level 关闭，倾向直接移除避免串口开销）。

## 假设与决策
- 崩溃循环中每次重启的 bash 以相同签名崩溃 ⇒ 信号源对每个新 bash 都成立 ⇒ 优先怀疑"死亡/复用槽残留的周期源"或"跨任务残留全局"，而非 bash 自身业务（bash 版本与工作负载 8 轮不变）。
- cr2=0x10206 极小 ⇒ 访问源自错误的寄存器/基址（fs 金丝雀 or 帧恢复的垃圾寄存器），排除普通堆越界。
- 插桩用 printk WARNING（现有用户 fault 同通道，可被测试脚本捕获）；投递/杀信号在正常负载下低频，不会破坏时序。
- 不触碰 brk/mmap/munmap 已定案改动；本次只动 signal/sched/exec 的清理与护栏。

## 实测取证结论（2026-09-03，已落代码，附于计划后）

### 崩溃现场（插桩 [crash]/[sigdbg]/[ustack]/[mem] 取证）
1. **首次崩溃不是信号**：run-9 命令 echo 后，bash（pid 4，fork 关键段，blk=0x14002=blocked {INT,TERM,CHLD}）`user fault rip=1 cr2=1`，现场 `pend=0 sigreq=0 h=0`——bash 执行一个被覆写成 **1** 的返回槽（[ustack] 首字=1，`ret` 到 1）。bash 自身栈/数据结构被**小整数覆写**。
2. **无限重投递循环（已修复）**：首次 SIGSEGV 投递给 bash 的 termsig handler（bash 确实给 SIGSEGV 注册了 termsig）→ handler 入口即崩（cr2=0x10206）→ 原代码无限重投递，栈每轮下移 144B，~30+ 行风暴。**修复后风暴中断**：`syscall_sig_active` 护栏让崩坏 handler 只再崩 1 次即被干净杀死 → 登录循环重启。
3. **[mem] used=129558/262144（约半满），全程无 [oom]** → **排除物理内存耗尽/泄漏假设**。
4. 全程无 [sigdbg] itimer / kill 行 → 排除 ITIMER_SIGALRM 与 sys_kill 为信号源。
5. fs_base=0x7f000035c740 正常 → 排除 fs/gs 残留假设。
6. **判别实验**：12× 轻量 `python3 -S -c "print(...)"` 全部通过（无 run9 崩溃）→ run-9 崩溃与 fork 周期数无关，由**重 import 负载**（子进程 dlopen/mmap 大量 .so）在父 bash 内存中诱发。

### 已落地修复（本会话，已编译验证）
1. **防无限重投递**（signal.c）：新增 `syscall_sig_active`——投递时置 1、rt_sigreturn 清 0；`signal_deliver_now`/`signal_check_deliver` 在 handler 执行期间拒绝嵌套投递。崩溃风暴 ~30 次 → 2 次干净收场。
2. **跨任务信号全局卫生**（signal.c `signal_globals_reset` + sched.c 挂载）：`sched_mark_exited`/`sched_kill`/`sched_spawn_process`（槽复用）/`sys_execve` 清零 `sig_request/handler/frame_rsp/frame_ptr/restore_*`——防子进程 arm 的请求在父进程 sysretq 被误消费。
3. **死亡任务定时器卫生**（sched.c）：mark_exited/kill 清 `itimer_deadline/interval`；`sched_poll_timeouts` 跳过 ZOMBIE/REAPED（不再对死进程触发/重排 SIGALRM）。
4. **spawn 槽复用**：复位 `fs_base/gs_base=0`。
（注：brk 收缩 unmap / mmap MAP_FIXED-unmap / skip-if-present 三项已在前序 brk/mmap 修复中经二分排除，未启用。）

### 遗留根因（run-9，未锁定，超出本计划取证范围）
bash 在重 import 子进程 fork 时刻，其自身栈/数据结构被小整数覆写（`ret` 到 1）。已排除：物理耗尽、内核信号源、fs/gs、fork 周期数。剩最大嫌疑：bash/glibc 在该负载下的 OOB 写，或某 syscall 在 8 次 dlopen 累积后对 bash 返回错误值。建议后续：
- QEMU `-d int,cpu_reset -D log` 抓 rip=1 前一条指令（bash 侧 call/ret 现场）；
- 或在 bash fork run-N 前后对 bash 用户栈/关键 .bss 打 canary 校验，二分覆写发生的 syscall 窗口；
- 或把 iso 用例降到"每轮 2 次 import"再跑 20 轮，找覆写出现的最小负载拐点。

### 验证状态
- 修复后：run1-8 全 OK，run9 单次崩 → 登录循环重启（风暴终止），与修复前 8/10 相同但系统可恢复。
- 轻量 python 12× 全过（判别实验）。
- 插桩打印（[sigdbg]/[crash]/[ustack]/[mem]/[oom]）保留在代码中便于后续取证；确认根因后应移除。
