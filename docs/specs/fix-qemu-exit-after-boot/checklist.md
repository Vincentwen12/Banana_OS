# Fix QEMU Exit After Boot Checklist

- [x] build.ps1 中 QEMU 命令行已移除 `-no-reboot`，保留 `-no-shutdown`
- [x] kmain.c 外层循环对所有 `sched_next()` 返回的任务都执行 `sched_requeue()`
- [x] kmain.c 内层键盘等待循环对所有 `sched_next()` 返回的任务都执行 `sched_requeue()`
- [x] sched.c 的 `sched_next()` 在 idle task 回退路径中设置 `current_task = idle_task`
- [x] 内核构建成功，kernel.bin < 80KB (74,872 bytes)
- [x] QEMU 启动后 Shell 提示符 `> ` 正常显示
- [x] 可以输入命令（如 `help`、`ps`）并获得响应
- [x] QEMU 不会在启动后立即退出