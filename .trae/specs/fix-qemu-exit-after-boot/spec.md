# Fix QEMU Exit After Boot Spec

## Why
内核启动后 QEMU 立即退出，Shell 提示符 `> ` 从未显示。根因是 `-no-reboot` 标志导致 QEMU 在 CPU 三重故障（triple fault）时退出，而内核 Shell 主循环中存在调度器死循环导致 triple fault。

## What Changes
- **build.ps1**: 移除 `-no-reboot`，保留 `-no-shutdown`，防止 QEMU 在 triple fault 时退出
- **kmain.c**: 修复 Shell 主循环中调度逻辑 — 对 `func=NULL` 的任务（Shell、Idle）执行后也需 requeue，避免任务卡在 RUNNING 状态
- **sched.c**: 修复 `sched_next()` 在返回 idle task 时未设置 `current_task` 的 bug

## Impact
- Affected specs: W3 (scheduler), W4 (power management)
- Affected code: build.ps1, src/kernel/kmain.c, src/kernel/sched/sched.c

---

## MODIFIED Requirements

### Requirement: QEMU 启动参数
QEMU 命令行 SHALL 移除 `-no-reboot` 标志，保留 `-no-shutdown`，确保 triple fault 时 QEMU 不会退出而是重启 VM。

#### Scenario: 内核 triple fault
- **WHEN** 内核发生 triple fault
- **THEN** QEMU 重启 VM（而非退出），用户可看到重启后的启动信息

### Requirement: Shell 主循环调度
Shell 主循环 SHALL 对所有 `sched_next()` 返回的任务执行 `sched_requeue()`，包括 `func=NULL` 的任务（Shell 任务、Idle 任务），防止任务卡在 RUNNING 状态导致调度器耗尽。

#### Scenario: Shell 任务被调度
- **WHEN** `sched_next()` 返回 Shell 任务（func=NULL）
- **THEN** Shell 任务被 requeue 回 MLFQ，state 从 RUNNING 变为 READY

#### Scenario: Idle 任务被调度
- **WHEN** `sched_next()` 返回 Idle 任务（func=NULL）
- **THEN** Idle 任务被 requeue 回 MLFQ，state 从 RUNNING 变为 READY

### Requirement: sched_next() 返回 idle task 时设置 current_task
`sched_next()` SHALL 在返回 idle task 时设置 `current_task = idle_task`，确保后续 `sched_tick()` 能正确递减 idle task 的时间片。

#### Scenario: idle task 被返回
- **WHEN** 所有 MLFQ 队列为空，`sched_next()` 返回 idle task
- **THEN** `current_task` 被设置为 idle_task，`sched_tick()` 能正确递减其时间片