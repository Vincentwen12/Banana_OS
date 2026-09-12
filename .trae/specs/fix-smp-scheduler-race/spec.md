# Fix SMP Scheduler Race Condition Spec

## Why
多核环境（`-smp 4`）下系统持续重启。根因是 AP 核心在 `ap_entry()` 的空闲循环中并发运行，与 BSP 无锁竞争共享状态：(1) 工程最初的设计是 AP 核心调用 `sched_tick()`/`sched_next()`/`sched_get_task()` 等共享调度器接口，与 BSP 的 Shell 主循环并发操作全局 MLFQ 队列和 `current_task`（无锁），破坏队列结构导致野指针访问；(2) `cstate.c` 的 `wakeup_addr`/`core_state` 是 .bss 全局变量（注释误判为 core-local），多个 AP 核心共享；且 `monitor`/`mwait` 指令在该无 IDT 的内核环境下触发异常即为 triple fault。上述任一竞态/异常都导致 triple fault；QEMU 命令行使用 `-no-shutdown` 而未加 `-no-reboot`，triple fault 时重启 VM，形成"一直重启"循环。

## What Changes
- **kmain.c**: `ap_entry()` 退化为最小空闲循环——上线通知 BSP 后直接 `while(1) hlt`，不再调用 `sched_tick()`/`sched_next()`/`sched_get_task()`，也不访问 doorbell、C-state、MWAIT 或 `timer_ms()` 等共享状态。AP 核心在真正具备 per-core 调度/IPC 前保持休眠。
- 调度器由 BSP 单核独占访问，无需引入自旋锁（避免过度设计）。

## Impact
- Affected specs: W3 (scheduler), W4 (power management), W5 (Linux ABI compat)
- Affected code: src/kernel/kmain.c

---

## MODIFIED Requirements

### Requirement: AP 核心空闲循环
AP 核心 SHALL 在 `ap_entry()` 中上线通知 BSP 后进入 `hlt` 最小空闲循环，不得调用 `sched_tick()`、`sched_next()`、`sched_get_task()`、`doorbell_poll()`、`cstate_*()` 或 `timer_ms()`，以避免与 BSP 并发访问共享状态。

#### Scenario: AP 核心进入空闲循环
- **WHEN** AP 核心启动完成并进入 `ap_entry` 的空闲循环
- **THEN** 核心执行 `hlt` 休眠，不操作全局 MLFQ 队列、doorbell 共享内存或 C-state 全局变量

#### Scenario: 多核启动后稳定运行
- **WHEN** 系统在 4 核（`-smp 4`）环境启动并持续运行
- **THEN** Shell 提示符 `>` 稳定显示、系统不重启，`run /bin/bash` 输出 "Hello from BananaOS ELF!"（仅一次引导，无 SeaBIOS 重复出现）

### Requirement: 调度器单核独占访问
全局调度器数据结构（MLFQ 队列、`current_task`、`task_table` 调度路径）SHALL 仅由 BSP 核心访问。

#### Scenario: BSP 独占调度
- **WHEN** 系统处于运行态
- **THEN** 仅 BSP 通过 Shell 主循环调用 `sched_next()`/`sched_tick()`/`sched_requeue()`，无其他核心并发操作