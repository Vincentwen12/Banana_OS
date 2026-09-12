# Axion-Ban Epoch v4.0 Spec

## Why
将 BananaOS 从 v1.1 确定性实时内核（无中断、无分页、无内核栈）重构为 v4.0 日常体验驱动内核，以"前台绝对优先、弹性内存、智能中断、安全不牺牲性能"为核心，实现开机快、响应跟手、内存弹性、省电安静的目标。

## What Changes
- **BREAKING**: 重构内核架构，从"无中断/无分页/无内核栈"改为"智能中断混合 + 分页安全隔离 + 内核栈支持"
- **BREAKING**: 替换 kmain.c 为新的内核入口，包含 Shell 交互循环
- 新增 Ω 弹性内存管理（位图分配 + SWAR 热度 + XOR 压缩）
- 新增 Ψ 公平调度器（5 级 MLFQ + 前台抢占）
- 新增 Δ 门铃 IPC 系统（64 通道 + ACL 权限掩码）
- 新增 Σ 功耗管家（动态中断/轮询切换 + CPU 频率导向）
- 新增交互式 Shell（`epoch>` 提示符，支持 help/mem/run/clear/reboot）
- 新增 ELF 加载器（静态链接，沙箱能力标签）
- 新增 POSIX 系统调用接口（约 40 个，musl 兼容）
- 新增三层安全隔离（物理分区 + 页表权限 + 门铃 ACL）
- 修改引导流程：保留 bootsect.S → boot.S → kmain 主链，boot.S 中启用分页（用于安全隔离而非虚拟内存）

## Impact
- Affected specs: 无（全新 spec）
- Affected code: 全部现有源文件需重构或替换
  - `src/boot/bootsect.S` - 保留，微调内核加载扇区数
  - `src/boot/boot.S` - 保留，启用分页（identity map + 权限位）
  - `src/kernel/kmain.c` - 完全重写，Shell 主循环
  - `src/kernel/vga.c` / `vga.h` - 保留并增强
  - `src/include/bananaos.h` - 扩展类型定义和常量
  - `linker.ld` - 更新内存布局
  - `Makefile` - 更新构建规则
  - 新增: `src/kernel/mm/` - 内存管理
  - 新增: `src/kernel/task/` - 调度器
  - 新增: `src/kernel/ipc/` - 门铃 IPC
  - 新增: `src/kernel/io/` - 键盘输入
  - 新增: `src/kernel/shell/` - Shell 命令

## ADDED Requirements

### Requirement: W1 - 闪电启动
系统 SHALL 在 QEMU 中启动，2 秒内显示 Shell 提示符 `epoch>`。

#### Scenario: 冷启动到 Shell
- **WHEN** 执行 `make run`
- **THEN** 串口输出显示 `BananaOS Axion-Ban Epoch v4.0`，然后显示 `epoch> ` 提示符
- **AND** 启动时间 < 2 秒

#### Scenario: 启动计时器输出
- **WHEN** 内核初始化完成
- **THEN** 串口输出启动耗时（毫秒级）

### Requirement: W1 - Shell 命令系统
系统 SHALL 提供交互式 Shell，支持基本命令。

#### Scenario: help 命令
- **WHEN** 用户输入 `help`
- **THEN** 显示所有可用命令列表及说明

#### Scenario: clear 命令
- **WHEN** 用户输入 `clear`
- **THEN** 清空 VGA 屏幕并重新显示提示符

#### Scenario: reboot 命令
- **WHEN** 用户输入 `reboot`
- **THEN** 系统通过键盘控制器复位 CPU

#### Scenario: mem 命令
- **WHEN** 用户输入 `mem`
- **THEN** 显示物理内存总量、已用量、可用量

#### Scenario: run 命令
- **WHEN** 用户输入 `run hello.elf`
- **THEN** 加载并执行 ELF 文件，输出其运行结果

#### Scenario: 未知命令
- **WHEN** 用户输入不存在的命令
- **THEN** 显示 `Unknown command` 错误提示

### Requirement: W1 - 键盘输入
系统 SHALL 通过 PS/2 键盘接收用户输入，支持回显、退格和回车。

#### Scenario: 基本输入回显
- **WHEN** 用户按下键盘按键
- **THEN** 对应字符显示在 VGA 屏幕上

#### Scenario: 退格键
- **WHEN** 用户按下退格键
- **THEN** 光标前移一位并清除该位置字符

#### Scenario: 回车键
- **WHEN** 用户按下回车键
- **THEN** 提交当前输入行给 Shell 解析

#### Scenario: 连续快速输入
- **WHEN** 用户连续快速按下 100 次按键
- **THEN** 所有字符均正确显示，无丢键

### Requirement: W2 - Ω 弹性内存管理
系统 SHALL 提供基于位图的物理内存分配器，支持分配、释放和统计。

#### Scenario: 内存分配
- **WHEN** 内核调用 `mm_alloc(n_pages)`
- **THEN** 返回连续物理页的起始地址

#### Scenario: 内存释放
- **WHEN** 内核调用 `mm_free(addr, n_pages)`
- **THEN** 对应物理页标记为可用

#### Scenario: mem 命令显示
- **WHEN** 用户执行 `mem` 命令
- **THEN** 显示 Total/Used/Free 内存统计（单位 MB）

#### Scenario: 内存耗尽处理
- **WHEN** 物理内存全部用完
- **THEN** `mm_alloc` 返回 NULL，系统不崩溃

### Requirement: W3 - Ψ 公平调度器
系统 SHALL 提供 5 级 MLFQ 调度器，支持前台优先抢占。

#### Scenario: 前台应用响应
- **WHEN** 后台运行 CPU 密集型任务
- **THEN** 前台 Shell 输入响应延迟 < 10ms

#### Scenario: 优先级老化
- **WHEN** 低优先级任务长时间未获得 CPU
- **THEN** 其优先级逐渐提升，防止饥饿

### Requirement: W3 - Δ 门铃 IPC
系统 SHALL 提供 64 通道门铃 IPC 机制，支持权限掩码控制。

#### Scenario: 门铃通知
- **WHEN** 进程 A 向通道 N 写入门铃
- **THEN** 订阅通道 N 的进程 B 被唤醒

#### Scenario: ACL 权限拒绝
- **WHEN** 进程尝试写入未授权的门铃通道
- **THEN** 返回 -EPERM 错误

### Requirement: W4 - Σ 功耗管家
系统 SHALL 在空闲时通过 HLT 指令降低功耗。

#### Scenario: 空载功耗
- **WHEN** 系统无任务运行
- **THEN** CPU 执行 HLT 指令，QEMU 主机侧 CPU 使用率 < 5%

### Requirement: W4 - 安全沙箱
系统 SHALL 对加载的 ELF 程序实施能力限制。

#### Scenario: 文件访问控制
- **WHEN** 沙箱程序尝试访问未授权路径
- **THEN** 返回 -EACCES 错误

#### Scenario: 门铃通道控制
- **WHEN** 沙箱程序尝试写入系统保留通道
- **THEN** 返回 -EPERM 错误

## MODIFIED Requirements
无（全新 spec，无现有需求修改）

## REMOVED Requirements
无（全新 spec，无现有需求移除）