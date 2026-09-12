# 修复 Demo 任务串口 'a' 噪音

## Summary
系统启动后 Shell 提示符 `>` 之后持续打印 `a`（以及潜在的 `b`），污染串口输出。根因是 `kmain.c` 中的两个调度器演示任务（DemoA、DemoB）在函数体内向串口输出心跳字符。移除这些调试输出，同时清理因此产生的未使用计数器，避免引入编译警告。

## Current State Analysis
[kmain.c](file:///d:/BananaOS-Axion/src/kernel/kmain.c) 中定义了两个演示任务：

- `demo_a_func()`（第 55-60 行）：每次调用 `demo_a_count++`，每当计数器达到 100000 的倍数时 `outb(SERIAL_PORT, 'a')`。这也是串口持续打印 `a` 的直接来源。
- `demo_b_func()`（第 63-72 行）：每次调用 `demo_b_count++`，轮询 doorbell 通道 1，收到消息时 `outb(SERIAL_PORT, 'b')` 并回写通道 2。

这两个函数被注册到调度器（第 177-187 行），在 Shell 主循环中被反复调度执行，导致串口噪音。`demo_a_count`、`demo_b_count` 仅在各自函数内自增/判断，无其他读取方。

## Proposed Changes

### 文件: src/kernel/kmain.c

1. 删除两个仅用于演示输出的计数器（第 50-52 行）：
   - `static int demo_a_count = 0;`
   - `static int demo_b_count = 0;`

2. `demo_a_func()` 改为无输出空函数（保留 DemoA 任务以维持调度器演示与 `ps` 可见性），去掉 `demo_a_count++` 与 `outb(SERIAL_PORT, 'a')`：
   ```c
   /* Demo task A: idle heartbeat (no output) */
   static void demo_a_func(void) {
   }
   ```

3. `demo_b_func()` 保留 doorbell IPC relay 逻辑，去掉 `demo_b_count++` 与 `outb(SERIAL_PORT, 'b')`：
   ```c
   /* Demo task B: IPC relay */
   static void demo_b_func(void) {
       /* Poll doorbell channel 1 for messages */
       uint64_t msg = doorbell_poll(1);
       if (msg != 0) {
           /* Echo back on channel 2 */
           doorbell_write(2, msg, 0);
       }
   }
   ```

> 不删除 DemoA/DemoB 任务本身，避免影响调度器现有行为与 `sched_list`/`ps` 输出结构。

## Assumptions & Decisions
- 用户抱怨的 `a` 为主噪源；同时清理同类的 `b` 输出，避免后续 doorbell 有消息时再次污染串口。
- 保留演示任务的注册（作为调度器的合法性演示），仅去除串口输出——最小且不改变任务调度语义。
- 删除计数器变量而非置空，避免 GCC `-Wall` 产生 "set but not used" 警告。

## Verification
1. 运行 `./build.ps1 run`，确认编译无新增警告（尤其无 `demo_a_count`/`demo_b_count` 未使用警告）。
2. 观察 4 核启动完成、`Hello from BananaOS ELF!` 输出后，Shell 提示符 `>` 之后不再持续打印 `a`/`b`。
3. 确认 `Booting from Hard Disk` 仅出现 1 次，系统不重启。