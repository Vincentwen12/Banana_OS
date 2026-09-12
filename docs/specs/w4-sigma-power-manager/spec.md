# W4 — Σ功耗管家完整版 Spec

## Why
W3 已完成 4 核多任务调度与门铃 IPC，但所有核心始终全速运行，功耗高、发热大。笔记本/桌面场景需要根据负载动态调节功耗——空闲时深度休眠省电，重载时全速响应。W4 为系统引入智能功耗管理，通过频率缩放、深度空闲、动态中断策略，在保证用户体验的前提下最小化能耗。

## What Changes
- **power/freq.c/h** (新增): CPU 频率缩放，通过 MSR 0x199/0x198 读写频率
- **power/cstate.c/h** (新增): 深度空闲 C-states，通过 MONITOR/MWAIT 指令进入低功耗
- **power/policy.c/h** (新增): 动态策略引擎，根据负载自动切换 IDLE/LIGHT/BALANCED/PERFORMANCE
- **sched.c** (修改): 注入空闲任务 (最低优先级)，无任务时触发 C-state
- **ap.c** (修改): AP 主循环集成功耗管理，空闲时进入 C-state
- **kmain.c** (修改): BSP 主循环集成策略引擎、负载采样
- **shell.c** (修改): 新增 `power` 命令 (显示频率/功耗/C-state/策略)
- **timer.c** (修改): 提供负载采样接口 `load_sample()`
- **axion.h** (修改): 新增频率/C-state/策略常量
- **Makefile** (修改): 新增 power/*.c 编译

## Impact
- Affected specs: w3-scheduler-ipc (调度器注入空闲任务，AP 主循环集成功耗)
- Affected code: sched.c, ap.c, kmain.c, shell.c, timer.c, axion.h, Makefile
- New files: src/kernel/power/freq.c, src/kernel/power/freq.h, src/kernel/power/cstate.c, src/kernel/power/cstate.h, src/kernel/power/policy.c, src/kernel/power/policy.h

## ADDED Requirements

### Requirement: CPU 频率缩放 (MSR 控制)
系统 SHALL 通过 IA32_PERF_CTL (MSR 0x199) 和 IA32_PERF_STATUS (MSR 0x198) 读写 CPU 频率。

#### Scenario: 查询当前频率
- **WHEN** 用户输入 `power status`
- **THEN** 显示当前频率 (kHz)、最低/最高频率范围

#### Scenario: 手动频率调节
- **WHEN** 用户输入 `power freq 2000000`
- **THEN** CPU 频率切换至 2.0GHz

#### Scenario: 频率缩放 API
- **WHEN** 系统调用 `freq_set(target_khz)`
- **THEN** 写入 MSR 0x199 设置目标频率，`freq_get()` 返回 MSR 0x198 当前频率

### Requirement: 深度空闲 C-state (MONITOR/MWAIT)
系统 SHALL 使用 MONITOR/MWAIT 指令集使核心进入低功耗状态，并可在门铃/中断到达时自动唤醒。

#### Scenario: 空闲时进入 C-state
- **WHEN** 系统无任务执行且持续 500ms
- **THEN** 调用 MONITOR/MWAIT，核心进入低功耗状态

#### Scenario: 门铃唤醒
- **WHEN** 核心处于 C-state 且门铃被写入
- **THEN** MONITOR 检测到缓存行修改，核心立即唤醒 (< 50μs)

#### Scenario: 浅睡眠 C1E
- **WHEN** 系统调用 `cstate_enter_light()`
- **THEN** 核心进入 C1E 状态，功耗降低约 30%

#### Scenario: 深睡眠
- **WHEN** 系统调用 `cstate_enter_deep()`
- **THEN** 核心进入深度 C-state，功耗降低约 80%

### Requirement: 动态策略引擎
系统 SHALL 根据负载自动切换运行策略 (IDLE / LIGHT / BALANCED / PERFORMANCE)。

#### Scenario: 空载 → IDLE
- **WHEN** 过去 100ms 负载为 0
- **THEN** 策略切换至 IDLE，仅 BSP 运行，AP 休眠，频率最低

#### Scenario: 轻载 → LIGHT
- **WHEN** 过去 100ms 负载 < 0.2
- **THEN** 策略切换至 LIGHT，BSP 运行，AP 轮询，频率中低

#### Scenario: 中载 → BALANCED
- **WHEN** 过去 100ms 负载 < 0.7
- **THEN** 策略切换至 BALANCED，所有核启用，频率动态调节

#### Scenario: 重载 → PERFORMANCE
- **WHEN** 过去 100ms 负载 >= 0.7
- **THEN** 策略切换至 PERFORMANCE，所有核全速，频率最高

#### Scenario: 策略切换滞后
- **WHEN** 策略发生切换
- **THEN** 新策略至少保持 100ms 才能再次切换，防止性能抖动

### Requirement: 任务空闲注入
系统 SHALL 在调度器无就绪任务时运行空闲任务，空闲任务触发功耗管理。

#### Scenario: 无就绪任务
- **WHEN** ready_mask == 0
- **THEN** 调度器执行 idle_task，idle_task 调用 `cstate_enter_deep()`

#### Scenario: 低优先级空闲
- **WHEN** 任何用户任务/内核任务就绪
- **THEN** 主动抢占空闲任务 (idle_task 优先级为最低 level 4)

### Requirement: Shell power 命令
系统 SHALL 在 Shell 中提供 `power` 命令，支持状态查看和策略手动切换。

#### Scenario: power status
- **WHEN** 用户输入 `power status`
- **THEN** 显示当前 CPU 频率 (kHz)、当前策略、各核心状态、负载统计

#### Scenario: power strategy <name>
- **WHEN** 用户输入 `power strategy performance`
- **THEN** 手动切换到 PERFORMANCE 策略，覆盖自动策略引擎

#### Scenario: power freq <khz>
- **WHEN** 用户输入 `power freq 2000000`
- **THEN** 强制 CPU 频率设置为 2.0GHz，覆盖策略

## MODIFIED Requirements

### Requirement: AP 主循环集成功耗
AP 核心主循环集成门铃轮询 + 空闲状态，无消息时进入 C-state。

#### Scenario: AP 空闲进入 C-state
- **WHEN** AP 核心无任务执行超过 50ms
- **THEN** 调用 `cstate_enter_deep_with_wakeup(doorbell_addr)`，由门铃写入唤醒

### Requirement: BSP 主循环集成功耗
BSP 主循环在键盘轮询 + 门铃轮询 + 调度执行之外，增加负载采样和策略切换。

#### Scenario: BSP 策略引擎
- **WHEN** BSP 主循环每次迭代
- **THEN** 采样负载 → 调用 `policy_apply(load)` → 无任务时调用 `cstate_enter_light()`

## Risk Mitigation
| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| MWAIT 唤醒延迟 > 50μs | 用户体验受影响 | W4 验收测量唤醒时间，若超标则缩短空闲阈值 (500ms → 50ms) |
| 频率 MSR 在 QEMU 不可用 | 测试阻塞 | QEMU 不支持频率缩放，使用模拟频率/占位值，真实硬件验证 |
| 核心进入 C-state 后收不到 IPI | 核间通信失效 | 门铃地址用 MONITOR 监听，写入触发缓存行一致性，硬件自动唤醒 |
| 策略切换过于频繁 | 性能抖动 | 加入滞后窗口 (策略切换后至少保持 100ms) |

## Code Size Estimate
| 文件 | 行数 |
|------|------|
| power/freq.c | 100 |
| power/cstate.c | 120 |
| power/policy.c | 100 |
| sched.c (修改) | +40 |
| ap.c (修改) | +30 |
| kmain.c (修改) | +20 |
| shell.c (修改) | +50 |
| axion.h (修改) | +20 |
| timer.c (修改) | +20 |
| **总计** | **~500 行** |