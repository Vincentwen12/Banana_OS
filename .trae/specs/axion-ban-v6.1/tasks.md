# Tasks: Axion-Ban v6.1

## W1: 引导 + VGA + 键盘 + 基础 Shell + Ω位图 + 调度骨架

### 基础设施
- [ ] Task 1: 创建项目基础设施
  - [ ] 创建 `linker.ld` 链接脚本（内核加载到 0x100000，ALIGN 4KB）
  - [ ] 创建 `src/include/axion.h` 全局头文件（类型定义、常量、内联工具）
  - [ ] 创建 `src/kernel/port.h` I/O 端口操作（inb/outb/rdtsc 等）
  - [ ] 创建 `iso/grub/grub.cfg` GRUB 引导配置
  - [ ] 创建 `Makefile` 构建系统（含 `make run`、`make debug`、`make iso`）

- [ ] Task 2: 实现 Multiboot 引导 + 长模式切换
  - [ ] 创建 `src/boot/boot.S`：Multiboot 头 + 32位入口 + 页表构建（identity map 4GB）+ 长模式切换
  - [ ] 验证 `make run` 能成功进入 64 位长模式并跳转到 kmain

### 核心驱动
- [ ] Task 3: 实现 VGA 文本模式驱动
  - [ ] 创建 `src/kernel/vga.c` 和 `src/kernel/vga.h`
  - [ ] 实现 vga_init()、vga_putc()、vga_puts()、vga_clear()
  - [ ] 支持光标移动、滚动、颜色设置
  - [ ] 同时输出到串口（COM1 0x3F8）用于调试

- [ ] Task 4: 实现 PS/2 键盘驱动
  - [ ] 创建 `src/kernel/keyboard.c` 和 `src/kernel/keyboard.h`
  - [ ] 实现 PS/2 键盘轮询读取（扫描码→ASCII 转换）
  - [ ] 支持 Shift 键状态追踪
  - [ ] 支持退格、回车、空格等特殊键
  - [ ] 实现 kbd_readline() 行输入（带回显）

- [ ] Task 5: 实现 TSC 时间基准
  - [ ] 创建 `src/kernel/timer.c` 和 `src/kernel/timer.h`
  - [ ] 实现 timer_init()（校准 TSC 频率）
  - [ ] 实现 timer_ms() 返回启动后毫秒数
  - [ ] 实现 timer_us() 返回启动后微秒数

### 内存管理
- [ ] Task 6: 实现 Ω 位图分配器
  - [ ] 创建 `src/kernel/mm.c` 和 `src/kernel/mm.h`
  - [ ] 实现 pmalloc()：使用 `__builtin_ctzll` 扫描位图分配单页
  - [ ] 实现 pfree(addr)：清除位图对应位
  - [ ] 实现 OOM 检测（位图全满时返回 NULL）
  - [ ] 实现 mm_stats()：返回 total/used/free 页数

- [ ] Task 7: 实现 SWAR 热度追踪骨架
  - [ ] 创建 `src/kernel/hotness.c` 和 `src/kernel/hotness.h`
  - [ ] 实现 4-bit 打包热度计数器（16 页/uint64_t）
  - [ ] 实现 hotness_update(page_idx)：页访问时热度+1
  - [ ] 实现 hotness_age()：全局右移老化（`>> 1 & 0x7777...`）

### 调度器
- [ ] Task 8: 实现 Ψ 调度器骨架
  - [ ] 创建 `src/kernel/sched.c` 和 `src/kernel/sched.h`
  - [ ] 定义 task_t 结构体（id, name, func）
  - [ ] 实现 ready_mask 位掩码（最多 64 任务）
  - [ ] 实现 sched_next()：返回下一个就绪任务 ID
  - [ ] 实现 sched_rotate()：循环左移 ready_mask

### IPC
- [ ] Task 9: 实现 Δ 门铃 IPC 骨架
  - [ ] 创建 `src/kernel/doorbell.c` 和 `src/kernel/doorbell.h`
  - [ ] 定义门铃数据结构（64 通道，缓存行对齐）
  - [ ] 实现 doorbell_write(channel, msg)：写入消息
  - [ ] 实现 doorbell_poll(channel)：轮询读取

### Shell
- [ ] Task 10: 实现无栈状态机 Shell
  - [ ] 创建 `src/kernel/shell.c` 和 `src/kernel/shell.h`
  - [ ] 实现命令解析器（空格分割）
  - [ ] 实现 `help` 命令（显示所有命令）
  - [ ] 实现 `hello` 命令（输出 "Hello, world!"）
  - [ ] 实现 `mem` 命令（调用 mm_stats 显示内存统计）
  - [ ] 实现 `alloc <N>` 命令（调用 pmalloc N 次）
  - [ ] 实现 `free <N>` 命令（调用 pfree N 次）
  - [ ] 实现 `clear` 命令（清屏）
  - [ ] 实现 `reboot` 命令（键盘控制器复位）
  - [ ] 实现 `panic` 命令（触发 kernel_panic）

### 错误处理
- [ ] Task 11: 实现内核错误处理
  - [ ] 创建 `src/kernel/panic.c` 和 `src/kernel/panic.h`
  - [ ] 实现 kernel_panic(msg)：显示错误信息，禁用中断，无限循环

### 内核入口
- [ ] Task 12: 实现内核入口 kmain
  - [ ] 创建 `src/kernel/kmain.c`
  - [ ] 按顺序初始化：vga → timer → mm → hotness → sched → doorbell → keyboard → shell
  - [ ] 输出启动 Banner："Axion-Ban Kernel v0.1"
  - [ ] 输出启动耗时
  - [ ] 进入 Shell 主循环（`> ` 提示符 + 命令执行）

### 构建验证
- [ ] Task 13: 构建验证
  - [ ] `make` 无警告，生成 `axion.iso`
  - [ ] `make run` 启动到 Shell 提示符
  - [ ] 所有 9 个内置命令可用
  - [ ] alloc/free 内存统计正确
  - [ ] panic 命令触发错误处理

## W2: Ω 弹性内存（完整版）

- [ ] Task 14: SWAR 热度接入 pmalloc
  - [ ] 在 pmalloc 中调用 hotness_update
  - [ ] 在 pfree 中重置热度计数器

- [ ] Task 15: 实现 XOR 差分压缩引擎
  - [ ] 创建 `src/kernel/compress.c` 和 `src/kernel/compress.h`
  - [ ] 实现 Per-Task 基准页存储
  - [ ] 实现 XOR 差分压缩（页 vs 基准页）
  - [ ] 实现解压（基准页 XOR 差异 = 原始页）

- [ ] Task 16: 实现回旋镖池
  - [ ] 实现三级冷数据下沉（热区→温区→回旋镖池）
  - [ ] 更新 mem 命令显示等效容量（物理 × 2.3）

- [ ] Task 17: alloc/free 压力测试
  - [ ] 实现循环 alloc/free 测试
  - [ ] 验证长时间运行无内存泄漏

## W3: Ψ 调度器 + Δ 门铃 IPC（完整版）

- [ ] Task 18: 实现 MLFQ 5 级调度器
  - [ ] 定义 5 级优先级队列（P0-P4）
  - [ ] 实现时间片轮转
  - [ ] 实现优先级老化（低优先级定期提升）
  - [ ] 实现上下文切换（保存/恢复寄存器）

- [ ] Task 19: 实现多核启动
  - [ ] 解析 ACPI/MADT 表获取 APIC ID
  - [ ] 实现 INIT-SIPI 启动序列
  - [ ] 实现 AP  Trampoline 代码

- [ ] Task 20: 实现门铃 ACL 权限控制
  - [ ] 添加 allowed_channels 掩码到 task_t
  - [ ] 实现门铃写入权限检查

## W4: Σ 功耗管家

- [ ] Task 21: 实现动态中断策略
  - [ ] 创建 `src/kernel/power.c` 和 `src/kernel/power.h`
  - [ ] 实现负载检测
  - [ ] 实现中断/轮询模式切换
  - [ ] 实现 CPU 深睡眠（HLT/MWAIT）

- [ ] Task 22: 实现 CPU 频率缩放
  - [ ] 通过 MSR 控制 CPU 频率
  - [ ] 根据负载自动调节

## W5-W6: Linux ABI 兼容层

- [ ] Task 23: 实现系统调用框架
  - [ ] 创建 `src/kernel/syscall.c` 和 `src/kernel/syscall.h`
  - [ ] 实现 syscall 入口（syscall 指令处理）
  - [ ] 实现系统调用分发表

- [ ] Task 24: 实现核心系统调用（约 40 个）
  - [ ] 进程管理：fork, execve, exit, wait4, getpid
  - [ ] 内存：brk, mmap, munmap
  - [ ] 文件 I/O：read, write, open, close, stat, fstat
  - [ ] 其他：gettimeofday, nanosleep, kill, signal

- [ ] Task 25: 实现 ELF 动态加载器
  - [ ] 解析 PT_INTERP
  - [ ] 加载动态链接库
  - [ ] 支持运行动态链接的 bash

- [ ] Task 26: 完善系统调用（约 150 个）
  - [ ] 补充剩余系统调用
  - [ ] 实现 /proc、/sys 仿真
  - [ ] 支持运行 gcc、python

## W7: 文件系统

- [ ] Task 27: 实现 VFS 接口
  - [ ] 创建 `src/kernel/fs/vfs.c` 和 `src/kernel/fs/vfs.h`
  - [ ] 定义 vfs_ops 结构体（mount, read, write, readdir 等）

- [ ] Task 28: 实现 EXT2/3/4 驱动
  - [ ] 手写 EXT2 超级块解析
  - [ ] 实现 inode 和目录遍历
  - [ ] 支持 EXT4 extent 格式

- [ ] Task 29: 实现 AxeFS 文件系统
  - [ ] 日志型结构（面向 SSD 优化）
  - [ ] 创建/读写/删除文件

## W8-W9: 自研桌面环境

- [ ] Task 30: 实现 Axion-WM 窗口管理器
  - [ ] 基础窗口创建和管理
  - [ ] 窗口拖拽、最小化、关闭
  - [ ] 桌面背景渲染

- [ ] Task 31: 实现 Axion-Files 文件管理器
  - [ ] 目录树浏览
  - [ ] 文件复制/移动/删除

- [ ] Task 32: 实现 Axion-Term 终端模拟器
  - [ ] 基于 Shell 的终端窗口
  - [ ] 字体渲染

- [ ] Task 33: 实现 Axion-Pkg 包管理器
  - [ ] 软件包索引
  - [ ] 一键安装/卸载

- [ ] Task 34: 实现 Axion-Settings 设置中心
  - [ ] 系统设置界面
  - [ ] 主题、网络、用户管理

## W10: 交付与部署

- [ ] Task 35: 实现 Live CD 模式
  - [ ] ISO 启动直接进入桌面
  - [ ] 无需安装即可使用

- [ ] Task 36: 实现一键安装器
  - [ ] 分区工具
  - [ ] 系统文件复制
  - [ ] 引导加载器安装

- [ ] Task 37: 24h 稳定性压测
  - [ ] 内存压力测试
  - [ ] 多任务并发测试
  - [ ] 文件系统完整性测试

# Task Dependencies
- Task 2 依赖 Task 1（需链接脚本和 Makefile）
- Task 3, 4, 5 互为并行（VGA、键盘、Timer 独立）
- Task 6 依赖 Task 1（需 axion.h 常量）
- Task 7 依赖 Task 6（热度追踪依赖页索引）
- Task 8, 9, 10, 11 互为并行（调度器、门铃、Shell、panic 独立）
- Task 12 依赖 Task 3, 4, 5, 6, 7, 8, 9, 10, 11（kmain 集成所有模块）
- Task 13 依赖 Task 12（构建验证）
- Task 14 依赖 Task 6, 7（SWAR 接入 pmalloc）
- Task 15 依赖 Task 6（压缩引擎依赖内存分配）
- Task 16 依赖 Task 14, 15（回旋镖池依赖热度和压缩）
- Task 17 依赖 Task 16（压力测试依赖完整内存系统）
- Task 18 依赖 Task 8（MLFQ 扩展调度骨架）
- Task 19 依赖 Task 18（多核依赖调度器）
- Task 20 依赖 Task 9（ACL 扩展门铃骨架）
- Task 21 依赖 Task 18（功耗管家依赖调度器状态）
- Task 22 依赖 Task 21（频率缩放依赖功耗策略）
- Task 23 依赖 Task 18, 20（系统调用依赖调度器和门铃）
- Task 24 依赖 Task 23
- Task 25 依赖 Task 24, 29（ELF 加载依赖文件系统）
- Task 26 依赖 Task 25
- Task 27 依赖 Task 6（VFS 依赖内存分配）
- Task 28 依赖 Task 27
- Task 29 依赖 Task 27
- Task 30-34 依赖 Task 28（桌面依赖文件系统）
- Task 35 依赖 Task 30-34（Live CD 依赖桌面）
- Task 36 依赖 Task 35（安装器依赖 Live CD）
- Task 37 依赖 Task 36（压测依赖完整系统）