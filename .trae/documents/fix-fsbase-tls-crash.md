# 修复 bash 外部命令后主进程崩溃（FS base 全局 MSR 被子进程覆盖）

## Summary

W7 Phase 3.2 目标（512MB 多组 fs.img + 完整工具链运行）已取得进展：bash 能启动、`ls` 等外部命令能运行并输出结果。但 **bash 主进程在 wait4 返回后崩溃**（用户态无限递归 #PF，CR2 固定 `0x7f000064c828`），导致 `python3 -V` / `vim --version` 等后续命令失败，W7 工具链冒烟测试不通过。

根因：**FS base（TLS 段基址）存在全局 MSR 中，未按任务保存/恢复**。bash fork 子进程 `ls`，`ls` 的 ld.so 通过 `arch_prctl(ARCH_SET_FS)` 设置自己的 TLS 基址，覆盖了全局 `MSR_FS_BASE`。bash 主进程 wait4 阻塞返回后，FS base 仍指向 `ls` 的 TLS 内存（`0x7f000064c828`），该地址在 bash 的地址空间中未映射 → bash 任何使用 `%fs:0x28` 的栈金丝雀检查（编译器 -fstack-protector 插入的函数序言）立即 #PF → 异常被当作 SIGSEGV → bash 崩溃。

本计划将该字段 per-task 化，并在任务切换/系统调用返回时恢复 MSR。

## Current State Analysis

### 相关代码路径

1. **`sys_arch_prctl`** — [table.c](file:///d:/BananaOS-Axion/src/kernel/syscall/table.c#L1488-L1514)
   - `ARCH_SET_FS (0x1002)` / `ARCH_SET_GS (0x1001)` 直接 `wrmsr(MSR_FS_BASE / MSR_GS_BASE)`，**未保存到 task**。
   - 两个 MSR 是 per-CPU 全局寄存器，任何任务调用都会覆盖。

2. **`task_t`** — [sched.h](file:///d:/BananaOS-Axion/src/kernel/sched/sched.h#L43-L126)
   - 无 `fs_base` / `gs_base` 字段。

3. **任务切换主循环** — [kmain.c](file:///d:/BananaOS-Axion/src/kernel/kmain.c#L301-L330)
   - 每次调度用户任务时切换 CR3（`vm_switch`），但**未恢复 MSR_FS_BASE/GS_BASE**。

4. **syscall 返回路径** — [syscall_entry.S](file:///d:/BananaOS-Axion/src/kernel/syscall/syscall_entry.S#L110-L137)
   - 阻塞唤醒（如 wait4 返回）后，代码从 `sched_block_and_switch` 恢复，继续执行 C 代码并最终 sysretq 回用户态。**此路径不经过主循环的任务启动点**，MSR 仍是被唤醒前最后一个运行任务（子进程）的值。

5. **fork** — [table.c](file:///d:/BananaOS-Axion/src/kernel/syscall/table.c#L203-L239)
   - 深拷贝地址空间，但不会复制 MSR（MSR 本来就该 per-task）。

### 崩溃证据链

- QEMU 日志：`v=0e`（#PF）cpl=3，IP=0x45ff24（bash 函数序言 `mov %fs:0x28,%rax`），CR2=0x7f000064c828。
- `0x7f000064c828` = 子进程 ls 的 TLS 基址 + 0x28（金丝雀槽）。
- bash 主进程 wait4 返回后 MSR_FS_BASE 仍 = ls 的 TLS 基址 → 读金丝雀 #PF。
- 单核/多核均复现（确定性 bug，非 SMP 竞争）。

## Proposed Changes

### 1. `task_t` 增加 FS/GS base 字段 — [sched.h](file:///d:/BananaOS-Axion/src/kernel/sched/sched.h#L43-L126)

在 `task_t` 中（信号字段附近）新增：

```c
    /* W7 Phase 3.2: 用户 TLS 段基址（arch_prctl 设置）。MSR_FS_BASE/GS_BASE
     * 是 per-CPU 全局寄存器，必须按任务保存并在切换/返回用户态时恢复，
     * 否则子进程覆盖后父进程读 %fs 金丝雀 #PF。 */
    uint64_t fs_base;
    uint64_t gs_base;
```

### 2. `sys_arch_prctl` 保存到任务 — [table.c](file:///d:/BananaOS-Axion/src/kernel/syscall/table.c#L1492-L1514)

`ARCH_SET_FS`/`ARCH_SET_GS` 分支中，在 `wrmsr` 前写入 `current_task`：

```c
    case 0x1001:  /* ARCH_SET_GS */
        wrmsr(MSR_GS_BASE, addr);
        if (current_task) current_task->gs_base = addr;
        return 0;
    case 0x1002:  /* ARCH_SET_FS */
        wrmsr(MSR_FS_BASE, addr);
        if (current_task) current_task->fs_base = addr;
        return 0;
```

### 3. fork 复制 FS/GS base — [table.c](file:///d:/BananaOS-Axion/src/kernel/syscall/table.c#L223-L239)

在 `sys_fork` 的继承区（uid/gid 之后）添加：

```c
    child->fs_base = current_task->fs_base;
    child->gs_base = current_task->gs_base;
```

### 4. 任务启动时恢复 MSR — [kmain.c](file:///d:/BananaOS-Axion/src/kernel/kmain.c#L308-L312)

主循环调度用户任务、`vm_switch` 之后：

```c
    wrmsr(MSR_FS_BASE, t->fs_base);
    wrmsr(MSR_GS_BASE, t->gs_base);
```

`MSR_FS_BASE (0xC0000100)` / `MSR_GS_BASE (0xC0000101)` 宏已在 table.c 中定义；kmain.c 需使用同一编号（内联汇编 `wrmsr`，参考 sys_arch_prctl 的写法）。若 kmain.c 未包含相应内联汇编帮助函数，直接写 `__asm__ volatile("wrmsr" :: "c"(msr), "a"(lo), "d"(hi) : "memory")`。

### 5. syscall 阻塞返回路径恢复 MSR（关键）

wait4/futex/select/sleep 等阻塞 syscall 返回用户态**不经过主循环启动点**，MSR 仍是其它任务的值。在 [syscall_entry.S](file:///d:/BananaOS-Axion/src/kernel/syscall/syscall_entry.S#L110-L113) 中，`call syscall_dispatch` 之后、`call signal_check_deliver` 之前，插入一次恢复：

在 syscall_entry.S 中新增 `.extern syscall_restore_msrs`，并在 dispatch 返回后调用。或更简单：在 `signal_check_deliver` 之前调用一个新增的 C 函数：

```asm
    call syscall_dispatch
    call syscall_restore_msrs        /* 恢复 current_task 的 FS/GS base */
    movq %rax, syscall_user_ctx+0(%rip)
    ...
```

在 [syscall.c](file:///d:/BananaOS-Axion/src/kernel/syscall/syscall.c) 中新增：

```c
void syscall_restore_msrs(void)
{
    if (!current_task) return;
    wrmsr(MSR_FS_BASE, current_task->fs_base);
    wrmsr(MSR_GS_BASE, current_task->gs_base);
}
```

（若 syscall.c 无 wrmsr 内联宏，与 table.c 保持一致实现。）

这样每次 syscall 返回用户态前 MSR 都恢复为当前任务的值，覆盖所有阻塞唤醒路径，同时消除对主循环启动点的依赖（方案 4 可作为双保险保留，或与方案 5 二选一；建议两者都做，成本极低）。

### 6. 清理临时调试文件（收尾）

删除本次排查产生的临时脚本/日志：
- `diag_libtinfo.ps1` / `diag_libtinfo.log`
- `diag_smp1.ps1` / `diag_smp1_stream.log`
- `diag_smp1_tool.ps1` / `smp1_tool.log`
- `diag_ud.ps1` / `qemu_ud.log`
- `diag_ls.ps1` / `ls_stream.log` / `qemu_ls.log`
- `dis_b1.txt` / `dis_b2.txt` / `dis_bash1.txt`

确认 `src/kernel/syscall/syscall.c` 的调试 include（`printk.h`）在无调试打印后无需保留则移除（`syscall_dispatch` 已恢复原样，若不再使用 printk 则删 include）。

## Assumptions & Decisions

- **FS base 是唯一根因**：崩溃证据（金丝雀 #PF + CR2 与子进程 TLS 一致）强指向 MSR 覆盖；本次 W7 冒烟已确认 bash 启动、ls execve、wait4 返回本身均正常，仅返回后 FS 错误。
- **方案 5（syscall 返回前统一恢复）为主**：覆盖所有阻塞唤醒路径，无需枚举每个 `sched_block_and_switch` 调用点。
- **GS base 一并处理**：成本为零，避免未来同类问题。
- 之前已完成的修复（`sys_newfstatat` 的 AT_EMPTY_PATH 语义、`elf_load` 用 `pmalloc_contig`、`sys_wait4` 保存/恢复 `syscall_user_rsp/ctx`）保持不变，它们是 bash 能运行到这一步的前提。

## Verification

1. 重建内核：`powershell -ExecutionPolicy Bypass -File .\build.ps1 all -SkipFs`
2. 运行完整 W7 工具链冒烟：`powershell -ExecutionPolicy Bypass -File .\test_w7_fsimg.ps1`
   - 预期全部 `OK`：bash 启动、ls 工具链路径、python3 -V、vim --version、cat passwd、bash 退出。
3. 回归：`powershell -ExecutionPolicy Bypass -File .\test_regression.ps1`（如存在且适用），确认无既有功能回归。
4. 清理临时调试文件（计划第 6 步）。
