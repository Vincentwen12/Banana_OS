# Fix SMP Scheduler Race Checklist

- [x] `ap_entry()` 不再调用 `sched_tick()`/`sched_next()`/`sched_get_task()`
- [x] `ap_entry()` 不再访问 `doorbell_poll()`/`cstate_*()`/`timer_ms()` 等共享状态，退化为 `while(1) hlt`
- [x] 编译通过，无针对 `ap_entry` 的未使用变量新警告
- [x] 4 核（`-smp 4`）启动后 Shell 提示符 `>` 稳定显示，系统持续运行不重启（`Booting from Hard Disk` 仅出现 1 次）
- [x] `run /bin/bash` 输出 "Hello from BananaOS ELF!"（无乱码、无重复字符）