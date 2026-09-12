# Tasks: Axion-Ban Epoch v4.0

## W1: 闪电启动

- [x] Task 1: 修复引导链并验证启动
  - [x] 修复 bootsect.S 的保护模式跳转（LGDT 地址修复 + 32位远跳转）
  - [x] 确保 boot.S 的 64 位长模式切换正常工作
  - [x] 调整内核加载扇区数（KERNEL_SECTORS 从 16 增加到 64，支持 32KB 内核）
  - [x] 在 boot.S 中启用分页（identity map 前 8MB，添加 U/S + R/W 权限位）
  - [x] 验证 `make run` 能成功启动到 kmain（启动输出 "1AGC2KL" 序列完整）

- [x] Task 2: 更新头文件和基础常量
  - [x] 更新 bananaos.h：添加 POSIX 类型（pid_t, ssize_t, off_t 等）、错误码常量、系统调用号
  - [x] 添加内存布局常量（KERNEL_HEAP_START, USER_SPACE_START 等）
  - [x] 添加门铃常量（DOORBELL_BASE, DOORBELL_COUNT 等）

- [x] Task 3: 实现 PS/2 键盘驱动
  - [x] 创建 src/kernel/io/kbd.c 和 kbd.h
  - [x] 实现 PS/2 键盘轮询读取（扫描码 → ASCII 转换）
  - [x] 支持 Shift 键状态追踪
  - [x] 支持退格(0x0E)、回车(0x1C)特殊键处理
  - [x] 实现输入缓冲区（环形缓冲区，256 字节）

- [x] Task 4: 实现 Shell 命令系统
  - [x] 创建 src/kernel/shell/shell.c 和 shell.h
  - [x] 实现命令解析器（空格分割命令和参数）
  - [x] 实现 `help` 命令（显示命令列表）
  - [x] 实现 `clear` 命令（清屏）
  - [x] 实现 `reboot` 命令（键盘控制器复位）
  - [x] 实现 `mem` 命令（显示内存统计，W2 完善）
  - [x] 实现 `run` 命令（加载 ELF，W3 完善）
  - [x] 实现 `echo` 命令（回显参数，用于测试）

- [x] Task 5: 重写 kmain.c 主循环
  - [x] 输出启动 Banner："BananaOS Axion-Ban Epoch v4.0"
  - [x] 输出启动耗时（使用 TSC 计时）
  - [x] 实现 Shell 主循环：显示 `epoch> ` 提示符 → 读取键盘输入 → 解析命令 → 执行
  - [x] 空闲时执行 HLT 降低功耗

- [x] Task 6: 更新构建系统
  - [x] 更新 Makefile：添加新源文件路径、增加 KERNEL_SECTORS
  - [x] 更新 linker.ld：调整内存布局（内核堆预留、BSS 扩展）
  - [x] 验证完整构建流程通过（kernel.raw = 4.5KB）

## W2: 弹性内存

- [ ] Task 7: 实现 Ω 位图物理内存分配器
  - [ ] 创建 src/kernel/mm/bitmap.c 和 bitmap.h
  - [ ] 实现 mm_init()：从 multiboot/e820 信息初始化位图
  - [ ] 实现 mm_alloc(n_pages)：使用 tzcnt 扫描位图，返回物理地址
  - [ ] 实现 mm_free(addr, n_pages)：清除位图对应位
  - [ ] 实现 mm_stats()：返回 total/used/free 页数

- [ ] Task 8: 完善 mem 命令
  - [ ] 更新 shell.c 中 mem 命令，调用 mm_stats() 显示 Total/Used/Free（MB）

- [ ] Task 9: 实现 SWAR 热度追踪（基础）
  - [ ] 创建 src/kernel/mm/swar.c 和 swar.h
  - [ ] 实现 4-bit 打包热度计数器
  - [ ] 实现单周期右移老化（全局热度衰减）

- [ ] Task 10: 实现 XOR 差分压缩（基础）
  - [ ] 创建 src/kernel/mm/compress.c 和 compress.h
  - [ ] 实现 Per-Task 基准页 + XOR 差异位图
  - [ ] 实现压缩/解压接口

## W3: 公平调度 + IPC

- [ ] Task 11: 实现 Ψ 5 级 MLFQ 调度器
  - [ ] 创建 src/kernel/task/sched.c 和 sched.h
  - [ ] 定义 task_struct（PID, 状态, 优先级, 时间片, 寄存器上下文）
  - [ ] 实现 5 级队列（P0-P4），每级轮转调度
  - [ ] 实现优先级老化（低优先级定期提升）
  - [ ] 实现上下文切换（保存/恢复寄存器）

- [ ] Task 12: 实现 Δ 门铃 IPC
  - [ ] 创建 src/kernel/ipc/doorbell.c 和 doorbell.h
  - [ ] 实现 64 通道门铃数组（volatile uint64_t，缓存行对齐）
  - [ ] 实现 doorbell_ring(channel, msg)：写入门铃并唤醒等待者
  - [ ] 实现 doorbell_wait(channel)：等待门铃通知
  - [ ] 实现 ACL 权限检查（allowed_channels 掩码）

- [ ] Task 13: 实现 ELF 加载器
  - [ ] 创建 src/kernel/task/elf.c 和 elf.h
  - [ ] 解析 ELF64 头，验证魔数和架构
  - [ ] 加载 LOAD 段到内存（mm_alloc + memcpy）
  - [ ] 设置入口地址并创建 task_struct
  - [ ] 注入沙箱能力标签（门铃通道掩码 + 文件路径前缀）

- [ ] Task 14: 完善 run 命令
  - [ ] 更新 shell.c 中 run 命令，从磁盘加载 ELF 并执行
  - [ ] 支持从 ramdisk 加载预置的测试 ELF 程序

## W4: 功耗管家 + 安全

- [ ] Task 15: 实现 Σ 动态中断策略
  - [ ] 创建 src/kernel/power/pm.c 和 pm.h
  - [ ] 实现负载检测（根据就绪任务数判断）
  - [ ] 实现中断/轮询模式切换（低负载时轮询门铃，高负载时启用中断）
  - [ ] 实现 CPU 空闲时 HLT 循环

- [ ] Task 16: 实现安全沙箱
  - [ ] 创建 src/kernel/task/caps.c 和 caps.h
  - [ ] 实现能力列表（capability list）结构
  - [ ] 实现系统调用入口的能力检查
  - [ ] 实现门铃通道 ACL 检查
  - [ ] 实现文件路径前缀检查

- [ ] Task 17: 实现 POSIX 系统调用表
  - [ ] 创建 src/kernel/syscall.c 和 syscall.h
  - [ ] 实现系统调用分发表（syscall_table[256]）
  - [ ] 实现 write/read/open/close/exit/fork 等核心调用桩
  - [ ] 实现 syscall 入口（int 0x80 或 syscall 指令处理）

# Task Dependencies
- Task 2 依赖 Task 1（需先验证启动链）
- Task 3, 4, 5 互为并行（键盘、Shell、kmain 可同时开发）
- Task 6 依赖 Task 2-5（构建系统需等所有源文件创建）
- Task 7 依赖 Task 1（需 multiboot 内存信息）
- Task 8 依赖 Task 7（mem 命令需内存统计）
- Task 9, 10 并行（SWAR 和压缩独立）
- Task 11 依赖 Task 7（调度器需内存分配）
- Task 12 并行于 Task 11（门铃独立于调度器）
- Task 13 依赖 Task 7, 11（ELF 加载需内存 + 任务创建）
- Task 14 依赖 Task 13（run 命令需 ELF 加载器）
- Task 15 依赖 Task 11（功耗管家需调度器状态）
- Task 16 依赖 Task 12, 13（沙箱需门铃 ACL + ELF 加载器）
- Task 17 依赖 Task 11, 12, 16（系统调用需调度器 + 门铃 + 沙箱）