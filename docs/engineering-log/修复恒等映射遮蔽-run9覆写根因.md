# 修复内核恒等映射被用户空间遮蔽（run9 fork 覆写 + 内核栈损坏）

## 摘要

run9 覆写源已**实锤定位**：不是分配器误释放、不是 vm_fork 深拷贝缺陷，而是**内核「线性地址=物理地址」恒等访问假设被用户映射遮蔽**。内核通过恒等线性地址访问自己刚分配的物理页；当该物理页的线性地址恰好已被某个用户任务的映射占用时，内核的 `vm_zero`/`vm_copy` 写的是**那个用户页**，而不是目标物理页。

- run9 首发崩溃：fork 深拷贝分配游标首次越过 512MB，`pmalloc` 返回物理页 `0x20000000`，而其恒等线性 `0x20000000` 正是 bash PIE 文本的 user 映射槽（PTE→物理 `0x80a000`）→ `vm_zero`/`vm_copy` 把 `.dynstr` 内容写进 bash 文本物理页 → bash 前若干页被覆写。
- 已用 TCG 硬件写断点实证：`[watch] hit rip=1024a3 dr6=ffff0ff2`——命中地址 = `vm_fork` 内 `vm_zero` 清零循环（`0x1024a0 movb $0,(%rax)`），触发断点 = DR1 `0x20000000`（不是物理 `0x80a000`），写入者就是**内核自己**的线性访问。
- 已落地的第一道修复（`mm.c` 的 `paddr_linear_ok` 分配期遮蔽检查）**已消除 fork 覆写**：修复后 `iso_s_imports` 中 run1–9 的 `[forkchk] … mism=0`（此前 run9 必然 DIFF），无 `user fault`、无 bash 文本污染。
- 但同源问题在 run9 以**第二形态**暴露：内核栈（`pmalloc_contig(4)` 分配，物理=线性 `0x20400000`）被用户堆映射遮蔽，内核栈内容变成 Python 模块文本 → 内核 `#GP`（`exc_print_c` RIP=ASCII、RSP=`0x20403ba0`、栈转储为 `…EOGRAPH-…` 文本）→ 系统 halt。

根因是结构性的：**热区物理范围 `[0x800000, 0x40800000)`（= 8MB…1.03GB）与用户空间起始 `0x20000000`（512MB）重叠**（见 [axion.h](file:///d:/BananaOS-Axion/src/include/axion.h#L26-L40)）。只要内核物理分配越过 512MB，任何长期存在的内核内存（内核栈、页表页、vm_context、ELF 读缓冲等）都可能在其恒等线性地址被用户映射后失效。分配期检查无法覆盖“分配时未被遮蔽、之后被另一任务堆增长遮蔽”的内核栈。

**主修复**：把用户程序装载基址从 `0x20000000` 抬到内核全部物理区之上（`0x4000000000`，256GB），使**用户线性空间与内核恒等物理区永不重叠**；`paddr_linear_ok` 作为防御性兜底保留。

## 当前状态分析（基于取证）

### 已确认事实
1. 物理页 `0x80a000`（bash 文本页 0）内容在 run9 fork 内由 ELF 头变为 glibc `.dynstr`（`__vfork`、`openat64`、`rwlock_t`…）。
2. `[pfx]` 零命中 → 无人 `pfree(0x80a000)`；`[npLOW]/[tblLOW]` 在 fork 窗口零命中 → 分配器**没有**把 ≤16MB 低区页直接发给 vm_fork。
3. `[alias]` 证明 bash 页表中 `0x80a000` 仅有 `v=0x20000000` 一个映射，无别名。
4. `[watch]`（TCG 数据断点，已自测证明有效）在 run9 命中：`rip=0x1024a3`（`vm_fork` 的 `vm_zero` 清零循环）、`dr6=0xffff0ff2`（bit1 → **DR1=0x20000000** 触发）。
   → 结论：写入者是内核 `vm_zero`/`vm_copy` 的**恒等线性访问**，其目标线性地址 `0x20000000` 被 bash 文本 user 映射占用，实际落到物理 `0x80a000`。
5. `pmalloc` 顺序分配下，run1–8 的 fork 分配游标 <512MB（`np=0x1433000…0x1bcf1000`），run9 才越过 512MB（子页 `physc=0x1f99e000` 起，第 ~163 页即到 `0x20000000`）→ 完美解释“仅 run9 复现”。
6. `paddr_linear_ok` 修复后：run1–9 `forkchk mism=0`、无 `[watch]`、无 `user fault`（覆写已消除）。
7. 修复后 run9 出现内核 `#GP`：`RSP=0x20403ba0`（内核栈 = `pmalloc_contig(4)` 于 `0x20400000`，见 [sched.c](file:///d:/BananaOS-Axion/src/kernel/sched/sched.c#L594-L599)），栈内容为 Python 模块文本（`…EOGRAPH-139F914…`）→ 该内核栈的恒等线性被用户映射遮蔽，内核在用户 CR3 下用栈即读到用户数据 → `ret` 到 ASCII → `#GP` halt。
8. 内存区常量确认结构性重叠：`HEAP_BASE=0x800000`、`HOT_ZONE_SIZE=0x40000000`（1GB）→ 热区止于 `0x40800000`；用户程序基址 `0x20000000`（[loader.c](file:///d:/BananaOS-Axion/src/kernel/elf/loader.c#L115-L120)），落在热区内部。

### 为什么分配期检查不够
`paddr_linear_ok` 用**分配时刻的当前 CR3**判定遮蔽。内核栈在 `sched_spawn` 时（父任务上下文）分配，此刻其线性地址未被遮蔽；随后该任务（exec 后堆增长）或其他任务的用户堆增长到该线性地址，才发生遮蔽。故长期存在的内核内存（尤其是每任务内核栈）必须在**构造上**远离任何用户可映射的线性地址。

## 建议改动

### 改动 1（主修复）：抬升用户程序装载基址，消除与内核恒等物理区的重叠
文件：`src/kernel/elf/loader.c`（`elf_load` 内 `ctx->base`）
- 将 `ctx->base = 0x20000000;`（第 120 行）改为 `ctx->base = 0x4000000000ULL;  /* 256GB：内核所有物理区之上 */`。
- 同步改写第 115–119 行注释：说明内核物理区覆盖热区 `[0x800000,0x40800000)`、温区 `[0x40800000,0x60800000)`、回旋镖 `[0x60800000,0x70800000)`，以及 LAPIC `[0xC0000000,0x100000000)`；用户程序基址必须**高于全部内核物理区**并避开 LAPIC，否则用户映射会遮蔽内核恒等物理页（历史注释里 512MB 的取值即为本次 bug 来源）。
- 影响面（均无需改动，但需验证）：
  - `ET_DYN` 主程序经 `load_bias = ctx->base` 重定位；`ctx->brk_start/brk_end = max_vaddr`（≈`0x4000000000+…`），`sys_brk` 自该处向上扩展。
  - `PT_INTERP`（ld.so）由 `elf_load_interp` 在既有高位基址（`~0x7f0000000000`）加载，不受影响；libc/mmap 走 `MMAP_HINT_BASE=0x7f0000100000`，不受影响；用户栈 `USER_STACK_TOP=0x8000000000`，不受影响。
  - `sig_stub` 固定 `0x600000000000`，不受影响。
- 选择 256GB 的理由：远高于全部内核物理区（≤1.76GB，避开 LAPIC 3–4GB），又远低于 libc mmap 提示区（≈139TB）与用户栈顶（512GB），给 brk 增长留出充足且不冲突的空间。

### 改动 2（防御兜底）：保留 `paddr_linear_ok` 分配期遮蔽检查
文件：`src/kernel/mm/mm.c`
- 保留已加入的 `paddr_linear_ok()` 及 `pmalloc()`（缓存路径 + 扫描路径）、`pmalloc_contig()` 中的遮蔽跳过逻辑。
- 理由：改动 1 之后正常情况下不会触发；作为“未来布局回归/非常规映射”的兜底，保证内核永不把被遮蔽的物理页当作可直接线性访问的页返回。
- 无需再改动；仅确认其与 `pfree` 的位图/缓存一致性（当前实现：遮蔽候选页置位占用并跳过，不泄漏重复分配）。

### 改动 3：移除全部诊断插桩
诊断插桩会显著拖慢（`forkchk_verify` 每次 fork 全量 memcmp 2147 页；`[watch]` 长期占用调试寄存器 DR0/DR1），且写死 `0x20000000`，抬升基址后语义失效，必须清除：
- `src/kernel/syscall/table.c`
  - 删除 `forkchk_phys`、`forkchk_verify`、`forkchk_alias` 三个静态函数（约第 236–362 行）。
  - 删除 `sys_fork` 内 `[forkchk] PRE` 快照块、`[watch] dbg_watch_arm` 调用、`forkchk_verify(...)` 调用（约第 384–420 行）。
- `src/kernel/mm/vm.c`
  - 删除 `[tblLOW]`（`alloc_table` 内，约 48–56 行）、`[npLOW]`（`vm_fork` 拷贝循环内，约 231–241 行）、`[vfork_end]`（约 250–266 行）。
  - 删除为打印新增的 `#include "printk.h"`（若删除后 vm.c 不再使用 printk）。
- `src/kernel/mm/mm.c`
  - 删除 `pfree` 内 `[pfx]` 诊断块（约第 188–199 行）。
- `src/kernel/core/idt.c`
  - 删除 `[watch]` 相关：`dbg_watch_active`、`dbg_print_watch`、`dbg_watch_arm`、`dbg_watch_disarm`（约 94–125 行）。
  - 删除 `exc_kill_user` 内 `[sigdbg]/[crash]`、`[ugpr]`、`[mem]`、`[ustack]` 打印块（约 138–185 行），仅保留 `user fault vec=… sig=… pid=… rip=… cr2=…` 一行与 `signal_deliver_now` / `sched_mark_exited` 逻辑。
  - 删除仅为 `[ugpr]` 引入的 `extern uint64_t fault_gpr[15];`。
- `src/kernel/core/idt.S`
  - 删除 `[watch] #DB` 分支（约 110–122 行）与 `.extern dbg_print_watch`。
  - 删除 `fault_gpr` 数组定义及其 GPR 保存块（`exc_common` 入口的 15 条 `movq …, fault_gpr+…`），恢复 `exc_common` 仅做取向量/构帧。
- `src/kernel/syscall/signal.c`
  - 删除 `[sigdbg] deliver` 打印（约 109–113 行）与 `[sigdbg] kill` 打印（约 256–261 行）；保留 `syscall_sig_active` 护栏、`signal_globals_reset` 等既有真实修复。
- `src/kernel/sched/sched.c`
  - 删除 `[sigdbg] itimer` 打印（约 374–377 行）；保留 itimer 到期清理与信号护栏逻辑（这些是本轮之前的定案修复）。
- 顺带修正失效注释：`src/kernel/syscall/table.c` 第 ~103 行 `sys_mmap` 注释中 “main program at 0x20000000” 改为与新基址一致或泛化为“主程序基址”。

### 不改动（已定案，保持）
- `mm.c`/`vm.c` 的 brk/mmap 碰撞避让、真实 `vm_unmap`（munmap 生效）。
- `signal.c` 的 `syscall_sig_active` 护栏、`signal_globals_reset`、任务退出/槽复用/exec 卫生。
- `sched.c` 的死任务 itimer 清零。
- `ata.c` 重试、`idt.S` 的最小 IDT/异常打印框架（仅删诊断附加值）。

## 假设与决策
- **假设**：rootfs 中 `/bin/bash`、`/usr/bin/python3` 等为 `ET_DYN`（PIE，Debian 默认），基址抬升对它们生效。**验证门**：实施前用 `readelf -h tools/rootfs/bin/bash tools/rootfs/usr/bin/python3` 确认 `Type: DYN`；若发现 `EXEC` 固定低地址的二进制，则暂停并单独评估（本计划不覆盖该分支）。若 `hello` 等自研测试 ELF 为 `ET_EXEC`，其固定低地址（通常 <0x800000）不与内核热区冲突，无需处理。
- **决策**：选择“抬升用户基址”而非“缩小热区/仅限制内核栈低地址”，因为用户 brk 无固定上界，任何固定热区上限都可能再次被越过；抬升基址是唯一有界且结构性的修复。
- **决策**：保留 `paddr_linear_ok` 作兜底（廉价、正常不触发），不因“主修复已足够”而删除。
- **决策**：诊断插桩全部移除，不保留“可选开关”，避免再次写死旧基址造成误导。

## 验证步骤
1. （门禁）确认 `bash`/`python3` 为 `ET_DYN`（见假设）。
2. 实施改动 1（可选先保留插桩观察一次，或直接连同改动 3 一起清理）；`build.ps1 -SkipFs` 编译通过。
3. 跑 `powershell -NoProfile -ExecutionPolicy Bypass -File .\iso_s_imports.ps1`：目标 **run 1..10 全部 `ok`（10/10）**；日志中不得出现 `[forkchk] DIFF`、`user fault`、`KERNEL EXCEPTION`/`System halted`、`[watch]`。
   - 说明：清理插桩后 `forkchk` 的全量 memcmp 开销消失，此前 run9 的 `timeout`（仅超时非崩溃）应一并消失。
4. 跑 `test_bash_restart.ps1`：bash 退出后登录循环重启正常（回归既有信号修复）。
5. 冒烟回归（按需，至少一项）：`test_shell_ux.ps1` 或 `test_w7_fsimg.ps1`，确认 shell 命令/工具链未因基址变化受损。
6. 复核 `kernel.flat` 体积未因新增代码显著变化（改动以删除为主）。

## 影响范围
- 修改：`src/kernel/elf/loader.c`（基址 + 注释）。
- 清理：`src/kernel/syscall/table.c`、`src/kernel/mm/vm.c`、`src/kernel/mm/mm.c`、`src/kernel/core/idt.c`、`src/kernel/core/idt.S`、`src/kernel/syscall/signal.c`、`src/kernel/sched/sched.c`。
- 保留（本计划已含的真修复）：`src/kernel/mm/mm.c`（`paddr_linear_ok`）、`src/kernel/mm/vm.c`（`vm_unmap`）、`src/kernel/syscall/table.c`（mmap 碰撞避让/真 munmap）、`src/kernel/syscall/signal.c`（信号护栏/卫生）。

## 回滚
- 若抬升基址引入新回归：仅需把 `ctx->base` 恢复为 `0x20000000`，并**保留** `paddr_linear_ok` 兜底（此时 run9 fork 覆写仍被兜底消除，但 run9 内核栈遮蔽问题会残留）。
- 若清理插桩导致难以定位后续问题：诊断插桩的内容可由本计划文件与 git 历史恢复；不保留运行期开关。
