# Tasks: W3 — Ψ公平调度 + Δ门铃IPC 完整版

## Task 1: 升级 axion.h 常量 + linker.ld 适配
新增调度和 IPC 相关常量，添加 AP 启动 trampoline 段。

- [x] SubTask 1.1: axion.h 新增 `MLFQ_LEVELS 5`, `TASK_STATE_RUNNING/READY/BLOCKED/SLEEPING/ZOMBIE`, `TIME_SLICE_BASE 10`, `MAX_CORES 4`
- [x] SubTask 1.2: axion.h 新增 `AP_TRAMPOLINE_ADDR 0x8000`, `LAPIC_BASE 0xFEE00000`, `IOAPIC_BASE 0xFEC00000`
- [x] SubTask 1.3: axion.h 新增 `IPC_SHM_BASE`, `IPC_SHM_SIZE`, `IPC_MAX_CHANNELS 16`
- [x] SubTask 1.4: linker.ld 新增 `.ap_trampoline` 段，起始地址 0x8000，确保 4KB 对齐

## Task 2: 升级 sched.c/h 为 MLFQ 5级调度器
从简单 round-robin 升级为 5 级多级反馈队列。

- [x] SubTask 2.1: 重定义 `task_t` 结构体 — 新增 state, priority, time_slice, ticks_left, core_id, doorbell_wait 字段
- [x] SubTask 2.2: 新增 `mlfq_t` 结构体 — 每级队列的 task 指针数组 + head/tail
- [x] SubTask 2.3: 实现 `sched_init()` — 初始化 5 级队列，注册 Shell 任务为最高优先级
- [x] SubTask 2.4: 实现 `sched_add(task)` — 按优先级入队，更新就绪状态
- [x] SubTask 2.5: 实现 `sched_next()` — 从高到低扫描非空队列，返回下一个任务
- [x] SubTask 2.6: 实现 `sched_tick()` — 减少当前任务 ticks_left，耗尽时降级优先级
- [x] SubTask 2.7: 实现 `sched_block(task, doorbell_ch)` — 任务阻塞等待门铃
- [x] SubTask 2.8: 实现 `sched_wake(task)` — 任务唤醒，恢复原优先级
- [x] SubTask 2.9: 实现 `sched_boost(task)` — 临时提升任务到最高优先级（用于键盘输入）
- [x] SubTask 2.10: 实现 `sched_kill(task_id)` — 标记任务为 ZOMBIE，从队列移除
- [x] SubTask 2.11: 实现 `sched_list(buf, max)` — 遍历所有任务，返回格式化的任务列表字符串

## Task 3: 升级 doorbell.c/h 为 ACL 权限 + 跨核通知
从裸通道升级为 ACL 权限控制 + 跨核 IPI 通知。

- [x] SubTask 3.1: 重定义 `doorbell_channel_t` — 新增 acl_mask, owner_id, ring_buf[16], rpos/wpos
- [x] SubTask 3.2: 实现 `doorbell_init()` — 初始化所有通道的 ACL 和环形缓冲区
- [x] SubTask 3.3: 实现 `doorbell_acl_set(channel, task_id, allow)` — 设置通道 ACL 权限
- [x] SubTask 3.4: 实现 `doorbell_write(channel, msg)` — 检查 ACL 权限，写入环形缓冲区
- [x] SubTask 3.5: 实现 `doorbell_poll(channel)` — 从环形缓冲区读取消息，检查是否有等待任务并唤醒
- [x] SubTask 3.6: 实现 `doorbell_send_ipi(core_id, vector)` — 通过 LAPIC ICR 发送核间中断
- [x] SubTask 3.7: 实现 `doorbell_notify(channel)` — 通知等待该通道的阻塞任务

## Task 4: 创建 ap.c/h — 多核 AP 启动
实现 INIT-SIPI-SIPI 序列唤醒 AP 核心。

- [x] SubTask 4.1: 创建 `src/kernel/ap.c` 和 `src/kernel/ap.h`
- [x] SubTask 4.2: 实现 `ap_trampoline` 汇编入口（16-bit 实模式 → 32-bit 保护模式 → 64-bit 长模式），放在 `.ap_trampoline` 段
- [x] SubTask 4.3: 实现 `ap_init()` — 解析 ACPI MADT 表找到 Local APIC 条目，收集 APIC ID
- [x] SubTask 4.4: 实现 `ap_start_all()` — 对每个 AP 发送 INIT-SIPI-SIPI 序列，等待 AP 在线
- [x] SubTask 4.5: 实现 `ap_get_core_id()` — 通过 LAPIC ID 寄存器返回当前核心 ID
- [x] SubTask 4.6: 实现 `ap_online_count()` — 返回已启动的 AP 数量

## Task 5: 创建 ipc.c/h — 进程间通信
实现共享内存 + 门铃通知的 IPC 机制。

- [x] SubTask 5.1: 创建 `src/kernel/ipc.c` 和 `src/kernel/ipc.h`
- [x] SubTask 5.2: 实现 `ipc_init()` — 分配共享内存区域
- [x] SubTask 5.3: 实现 `ipc_send(channel, data, len)` — 将数据拷贝到共享内存，通过门铃通知接收方
- [x] SubTask 5.4: 实现 `ipc_recv(channel, buf, len)` — 从共享内存读取数据
- [x] SubTask 5.5: 实现 `ipc_create_channel(task_a, task_b)` — 创建双向通信通道

## Task 6: 更新 kmain.c 多核入口
BSP/AP 分叉执行路径，集成调度器主循环。

- [x] SubTask 6.1: 在 kmain 入口处调用 `ap_get_core_id()` 判断当前核心
- [x] SubTask 6.2: BSP 路径：保持现有初始化流程，新增 `ap_start_all()` 调用
- [x] SubTask 6.3: BSP 路径：Shell 循环中集成 `sched_tick()` 和 `sched_next()` 交替执行
- [x] SubTask 6.4: AP 路径：跳过 PIC/VGA 初始化，直接进入 `sched_loop()` 工作循环
- [x] SubTask 6.5: 实现 `sched_loop()` — 无限循环：选任务 → 执行 → tick → 旋转

## Task 7: 更新 shell.c 新增命令
新增 ps、kill、ipc 命令。

- [x] SubTask 7.1: 实现 `cmd_ps()` — 调用 `sched_list()` 显示所有任务
- [x] SubTask 7.2: 实现 `cmd_kill()` — `kill <task_id>` 终止任务
- [x] SubTask 7.3: 实现 `cmd_ipc()` — `ipc <channel> <msg>` 发送 IPC 测试消息
- [x] SubTask 7.4: 在 `shell_init()` 注册 `ps`, `kill`, `ipc` 命令
- [x] SubTask 7.5: Shell 键盘输入时调用 `sched_boost(shell_task)` 提升优先级

## Task 8: 更新 build.ps1 支持 SMP
QEMU 多核启动，新增 ap.c/ipc.c 编译。

- [x] SubTask 8.1: 编译列表新增 `src/kernel/ap.c`, `src/kernel/ipc.c`
- [x] SubTask 8.2: QEMU run/run-gui/debug 命令新增 `-smp 2` 参数
- [x] SubTask 8.3: AP trampoline 代码特殊处理（16-bit 汇编，需单独编译）

## Task 9: 构建验证 + 启动测试
确认编译通过、QEMU 多核启动正常、所有命令可用。

- [ ] SubTask 9.1: `.\build.ps1 all` 编译无错误
- [ ] SubTask 9.2: `.\build.ps1 run` 双核启动到 Shell 提示符
- [ ] SubTask 9.3: `ps` 命令显示任务列表
- [ ] SubTask 9.4: `ipc 0 hello` 发送 IPC 测试消息
- [ ] SubTask 9.5: `kill <id>` 终止任务
- [ ] SubTask 9.6: Core 1 输出 "AP Core 1 online" 日志

# Task Dependencies
- Task 2 依赖 Task 1（sched 需 axion.h 常量和结构体定义）
- Task 3 依赖 Task 1（doorbell 需 axion.h 常量）
- Task 4 依赖 Task 1（ap 需 linker.ld 段和 LAPIC 常量）
- Task 5 依赖 Task 3（ipc 依赖 doorbell 通知）
- Task 6 依赖 Task 2, 4（kmain 需调度器和 AP 启动）
- Task 7 依赖 Task 2, 3, 5（Shell 命令需调度器、门铃、IPC 接口）
- Task 8 依赖 Task 4（build 需 ap.c 文件就位）
- Task 9 依赖 Task 1-8（全量验证）