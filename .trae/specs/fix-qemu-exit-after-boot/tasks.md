# Tasks

- [x] Task 1: 修复 build.ps1 — 移除 `-no-reboot` 标志
  - [x] 在 QEMU 命令行中移除 `-no-reboot`，保留 `-no-shutdown`
  - [x] 验证：构建并运行，确认 QEMU 不会在 triple fault 时退出

- [x] Task 2: 修复 kmain.c Shell 主循环 — 所有任务都 requeue
  - [x] 修改外层循环的调度逻辑：移除 `t->func` 检查，对所有 `sched_next()` 返回的有效任务都执行 `sched_requeue()`
  - [x] 修改内层键盘等待循环的调度逻辑：同上
  - [x] 验证：构建并运行，确认 Shell 提示符 `> ` 正常显示

- [x] Task 3: 修复 sched.c — `sched_next()` 返回 idle task 时设置 `current_task`
  - [x] 在 `sched_next()` 的 idle task 回退路径中添加 `current_task = idle_task`
  - [x] 验证：构建并运行，确认 idle task 时间片正常递减和重新入队

# Task Dependencies
- Task 2 依赖 Task 3（kmain 的 requeue 逻辑依赖 sched_next 正确设置 current_task）
- Task 1 可并行执行