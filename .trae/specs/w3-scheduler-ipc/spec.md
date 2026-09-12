# W3 — Ψ公平调度 + Δ门铃IPC 完整版 Spec

## Why
当前内核是单核单任务轮询模型，无法利用多核CPU，也无法支持多任务并发。W3 将调度器从简单轮转升级为 5 级 MLFQ（多级反馈队列），门铃从裸通道升级为 ACL 权限控制 + 跨核通知，并实现多核 AP 启动和进程间通信。

## What Changes
- **sched.c/h**: 从简单 round-robin 升级为 5 级 MLFQ，支持优先级提升、时间片轮转、任务阻塞/唤醒
- **doorbell.c/h**: 新增 ACL 权限掩码、跨核 IPI 通知、环形缓冲区
- **ap.c/h** (新增): 多核 AP 启动，通过 INIT-SIPI 序列唤醒 AP，每核独立运行
- **ipc.c/h** (新增): 进程间通信，共享内存 + 门铃通知
- **kmain.c**: 多核主循环分支（BSP 跑 Shell，AP 跑工作循环）
- **shell.c**: 新增 `ps`、`kill`、`ipc` 命令
- **build.ps1**: QEMU 新增 `-smp 2` 支持多核
- **axion.h**: 新增调度/IPC 相关常量
- **linker.ld**: 新增 AP 启动代码段

## Impact
- Affected specs: w2-omega-memory (无冲突，调度器在内存管理之上)
- Affected code: sched.c, sched.h, doorbell.c, doorbell.h, kmain.c, shell.c, axion.h, build.ps1, linker.ld
- New files: src/kernel/ap.c, src/kernel/ap.h, src/kernel/ipc.c, src/kernel/ipc.h

## ADDED Requirements

### Requirement: MLFQ 5级调度器
系统 SHALL 提供 5 级多级反馈队列调度器，支持时间片轮转、优先级提升和任务阻塞。

#### Scenario: 多任务轮转
- **WHEN** 系统注册 3 个后台任务
- **THEN** `ps` 命令显示 3 个任务交替运行，每个任务获得公平的 CPU 时间

#### Scenario: 前台加速
- **WHEN** 用户键盘输入时
- **THEN** Shell 任务优先级自动提升到最高级，后台任务不阻塞输入响应

#### Scenario: 任务阻塞与唤醒
- **WHEN** 任务等待门铃消息时调用 `sched_block()`
- **THEN** 任务进入 BLOCKED 状态，不再参与调度；当门铃消息到达时调用 `sched_wake()` 恢复

#### Scenario: 时间片耗尽
- **WHEN** 当前任务时间片用完
- **THEN** 任务降级到下一优先级队列，调度器选择下一个就绪任务

### Requirement: 门铃 ACL 权限控制
系统 SHALL 为每个门铃通道提供 64-bit ACL 权限掩码，仅授权任务可写入。

#### Scenario: 授权写入
- **WHEN** 任务 A 对通道 0 有写入权限
- **THEN** `doorbell_write(0, msg)` 成功写入

#### Scenario: 未授权拒绝
- **WHEN** 任务 B 对通道 0 无写入权限
- **THEN** `doorbell_write(0, msg)` 静默丢弃，不写入

#### Scenario: 跨核门铃
- **WHEN** Core 0 通过门铃通道发送消息给 Core 1
- **THEN** Core 1 轮询到消息并返回确认

### Requirement: 多核 AP 启动
系统 SHALL 通过 INIT-SIPI-SIPI 序列启动 AP（Application Processor），使每个核独立运行。

#### Scenario: 双核启动
- **WHEN** QEMU 以 `-smp 2` 启动
- **THEN** Core 0 (BSP) 输出初始化日志，Core 1 (AP) 输出 "AP Core 1 online" 日志

#### Scenario: 每核独立调度
- **WHEN** 多核启动后
- **THEN** 每个核有独立的调度器实例和任务队列

### Requirement: Shell 新增命令
系统 SHALL 在 Shell 中提供 `ps`、`kill`、`ipc` 命令。

#### Scenario: ps 命令
- **WHEN** 用户输入 `ps`
- **THEN** 显示所有任务列表，包含 ID、名称、状态、优先级、所属核心

#### Scenario: kill 命令
- **WHEN** 用户输入 `kill <task_id>`
- **THEN** 目标任务被标记为终止，不再参与调度

#### Scenario: ipc 命令
- **WHEN** 用户输入 `ipc <channel> <msg>`
- **THEN** 向指定门铃通道发送消息，用于测试 IPC

## MODIFIED Requirements

### Requirement: kmain 多核入口
kmain SHALL 根据当前核心 ID 分叉执行路径：BSP 运行 Shell 交互循环，AP 运行任务工作循环。

#### Scenario: BSP 路径
- **WHEN** Core 0 执行 kmain
- **THEN** 初始化所有子系统后进入 Shell 主循环

#### Scenario: AP 路径
- **WHEN** Core 1+ 执行 kmain
- **THEN** 跳过 PIC 禁用、VGA 初始化等 BSP 专属步骤，直接进入任务调度循环