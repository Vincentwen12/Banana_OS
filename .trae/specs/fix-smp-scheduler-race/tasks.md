# Tasks

- [x] Task 1: 移除 AP 核心对共享调度器及共享状态的访问
  - [x] 将 `ap_entry()` 的空闲循环（`src/kernel/kmain.c`）退化为最小 `while(1) hlt` 循环
  - [x] 删除对 `sched_tick()`/`sched_next()`/`sched_get_task()` 的调用
  - [x] 同时移除对 `doorbell_poll()`/`cstate_*()`/`timer_ms()` 等共享状态的访问（诊断发现这些同样引发竞态或 MWAIT 异常）
  - [x] 验证：`./build.ps1 run` 在 4 核（`-smp 4`）下稳定启动且不重启，`run /bin/bash` 输出 "Hello from BananaOS ELF!"

# Task Dependencies
- 无外部依赖，单任务即可完成修复