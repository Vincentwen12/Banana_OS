# Axion-Ban v6.1 完整实施计划 Spec

## Why
从零构建一个 100% 自研内核的操作系统，兼具 Linux 二进制兼容与开箱即用的桌面体验。v6.1 核心承诺：自研内核 + Linux ABI 兼容 + 桌面环境，20 周 10 阶段交付。

## What Changes
- **BREAKING**: 完全替换现有 v4.0 代码库，采用新架构（Multiboot 引导、axion.h 命名空间、新文件结构）
- 新增 Ω 弹性内存（位图分配 + SWAR 热度 + XOR 压缩 + 回旋镖池）
- 新增 Ψ 公平调度器（MLFQ 5 级 + 多核）
- 新增 Δ 门铃 IPC（64 通道 + ACL）
- 新增 Σ 功耗管家（动态中断 + 频率缩放）
- 新增 Linux ABI 兼容层（约 150 个系统调用）
- 新增 VFS + EXT2/3/4 + 自研 AxeFS
- 新增自研桌面环境（WM + 文件管理器 + 终端 + 包管理器 + 设置中心）
- 新增 Live CD + 一键安装器

## Impact
- Affected specs: 无（全新 spec，替换 axion-ban-epoch v4.0）
- Affected code: 全部重建
  - `src/boot/boot.S` - Multiboot 引导 + 长模式
  - `src/include/axion.h` - 全局类型定义
  - `src/kernel/port.h` - I/O 端口操作
  - `src/kernel/vga.c/h` - VGA 文本驱动
  - `src/kernel/keyboard.c/h` - PS/2 键盘驱动
  - `src/kernel/timer.c/h` - TSC 时间基准
  - `src/kernel/mm.c/h` - Ω 位图分配器
  - `src/kernel/hotness.c/h` - SWAR 热度追踪
  - `src/kernel/sched.c/h` - Ψ 调度器
  - `src/kernel/doorbell.c/h` - Δ 门铃 IPC
  - `src/kernel/shell.c/h` - 无栈状态机 Shell
  - `src/kernel/panic.c/h` - 内核错误处理
  - `src/kernel/kmain.c` - 内核入口
  - `linker.ld` - 链接脚本
  - `Makefile` - 构建系统
  - `iso/grub/grub.cfg` - GRUB 引导配置

## ADDED Requirements

### Requirement: W1 - 引导与基础内核
系统 SHALL 通过 Multiboot 引导，进入 64 位长模式，identity map 4GB，提供交互式 Shell 和基础内存管理。

#### Scenario: 冷启动到 Shell
- **WHEN** 执行 `make run`
- **THEN** 2 秒内显示 "Axion-Ban Kernel v0.1" 和 Shell 提示符 `>`
- **AND** 键盘输入字符实时回显

#### Scenario: hello 命令
- **WHEN** 用户输入 `hello`
- **THEN** 输出 "Hello, world!"

#### Scenario: mem 命令
- **WHEN** 用户输入 `mem`
- **THEN** 显示物理内存总量、已用页、空闲页、等效容量

#### Scenario: alloc 命令
- **WHEN** 用户输入 `alloc 10`
- **THEN** 分配 10 个物理页，mem 显示已用页增加 10

#### Scenario: free 命令
- **WHEN** 用户输入 `free 10`
- **THEN** 释放 10 个物理页，mem 显示已用页减少 10

#### Scenario: panic 命令
- **WHEN** 用户输入 `panic`
- **THEN** 显示 `[PANIC] Manual panic`，系统暂停

#### Scenario: OOM 处理
- **WHEN** 物理内存耗尽
- **THEN** pmalloc 返回 NULL，系统显示 `[PANIC] Out of memory`

#### Scenario: 长时间稳定性
- **WHEN** 连续敲击 1000 次按键
- **THEN** 系统不崩溃，无内存泄漏

### Requirement: W2 - Ω 弹性内存（完整版）
系统 SHALL 提供 SWAR 热度追踪、XOR 差分压缩、回旋镖池三级存储。

#### Scenario: 等效容量
- **WHEN** 用户执行 `mem`
- **THEN** 等效容量约为物理内存 × 2.3

#### Scenario: alloc/free 压力测试
- **WHEN** 执行大量 alloc/free 循环
- **THEN** 系统不崩溃，内存正确回收

### Requirement: W3 - Ψ 调度器 + Δ 门铃 IPC（完整版）
系统 SHALL 提供 MLFQ 5 级调度、多核启动、核间门铃通信。

#### Scenario: 多任务并发
- **WHEN** 创建多个任务
- **THEN** 各任务交替执行，公平分享 CPU

#### Scenario: 多核启动
- **WHEN** 系统检测到多核 CPU
- **THEN** AP 通过 INIT-SIPI 启动并加入调度

### Requirement: W4 - Σ 功耗管家
系统 SHALL 在空载时关闭中断进入深睡眠，空载功耗 < 1.2W。

#### Scenario: 空载功耗
- **WHEN** 系统无任务运行
- **THEN** CPU 进入深睡眠状态

### Requirement: W5-W6 - Linux ABI 兼容层
系统 SHALL 提供约 150 个 Linux 兼容系统调用，支持运行动态链接的 bash、gcc、python。

#### Scenario: 运行 bash
- **WHEN** 加载静态编译的 bash
- **THEN** bash 正常运行，可执行内置命令

#### Scenario: 动态链接程序
- **WHEN** 加载动态链接的 ELF
- **THEN** PT_INTERP 解析正确，程序正常运行

### Requirement: W7 - 文件系统
系统 SHALL 支持 EXT2/3/4 读写和自研 AxeFS 日志型文件系统。

#### Scenario: 挂载 EXT4 分区
- **WHEN** 挂载现有 Linux EXT4 分区
- **THEN** 可读取文件列表和内容

### Requirement: W8-W9 - 自研桌面环境
系统 SHALL 提供窗口管理器、文件管理器、终端模拟器和包管理器。

#### Scenario: 图形界面操作
- **WHEN** 进入桌面环境
- **THEN** 窗口可拖拽、最小化、关闭

### Requirement: W10 - 交付与部署
系统 SHALL 提供 Live CD 模式和一键安装器。

#### Scenario: Live CD 启动
- **WHEN** 从 ISO 启动
- **THEN** 直接进入 Live 桌面，无需安装

## MODIFIED Requirements
无（全新 spec）

## REMOVED Requirements
无（全新 spec）