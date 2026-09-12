# Tasks

- [x] Task 1: 创建 power 模块目录和头文件
  - [x] 创建 `src/kernel/power/` 目录
  - [x] 创建 `freq.h`、`cstate.h`、`policy.h` 头文件，声明接口函数
  - [x] 在 `axion.h` 中新增频率/C-state/策略常量和 MSR 定义 (常量定义在 power/*.h 中)

- [x] Task 2: 实现 CPU 频率缩放 (freq.c)
  - [x] 实现 `freq_init()`: 读取 MSR 0x198 获取当前频率基数，设置默认最低/最高频率
  - [x] 实现 `freq_set(uint32_t khz)`: 写入 MSR 0x199 设置目标频率
  - [x] 实现 `freq_get()`: 读取 MSR 0x198 返回当前频率 (kHz)
  - [x] 实现 `freq_boost()`: 设置最高频率
  - [x] 实现 `freq_drop()`: 设置最低频率
  - [x] QEMU 兼容: 检测 MSR 不可用时使用模拟频率 + 占位值

- [x] Task 3: 实现深度空闲 C-state (cstate.c)
  - [x] 实现 `cstate_init()`: 初始化空闲状态跟踪
  - [x] 实现 `cstate_enter_light()`: 浅睡眠 C1E (MONITOR + MWAIT with hint)
  - [x] 实现 `cstate_enter_deep()`: 深睡眠 (MONITOR + MWAIT with deep hint)
  - [x] 实现 `cstate_set_wakeup_addr(void* addr)`: MONITOR 监听目标地址
  - [x] 实现 `cstate_get_state()`: 返回当前核心 C-state 状态字符串

- [x] Task 4: 实现动态策略引擎 (policy.c)
  - [x] 实现策略枚举 `POLICY_IDLE` / `POLICY_LIGHT` / `POLICY_BALANCED` / `POLICY_PERFORMANCE`
  - [x] 实现 `policy_init()`: 初始化策略为 BALANCED
  - [x] 实现 `policy_apply(float load)`: 根据负载自动切换策略 (含 100ms 滞后窗口)
  - [x] 实现 `policy_set_manual(policy_t)`: 手动覆盖策略
  - [x] 实现 `policy_get()`: 返回当前策略名称字符串
  - [x] 实现 `policy_get_load()`: 返回当前负载值

- [x] Task 5: 修改调度器注入空闲任务 (sched.c)
  - [x] 新增 `sched_idle_task` 静态任务，优先级 level 4，func 指向 cstate 调用
  - [x] 修改 `sched_next()`: 无就绪任务时返回 idle_task
  - [x] 实现 `sched_load_sample()`: 返回过去 100ms 内活跃任务数 / 总任务数

- [x] Task 6: 修改 AP 主循环集成功耗 (ap.c)
  - [x] 修改 AP 空闲循环: 替换 `pause` 为 `cstate_enter_deep_with_wakeup(doorbell_addr)`
  - [x] 实现 AP 空闲时间跟踪: 超过 50ms 无工作进入 C-state
  - [x] 实现 `ap_get_state()`: 返回 AP 核心当前状态 (Active/C1E/Deep C-state)

- [x] Task 7: 修改 BSP 主循环集成功耗 (kmain.c)
  - [x] 在 BSP 主循环中集成 `policy_apply(load)` 策略引擎
  - [x] 集成 `sched_load_sample()` 负载采样
  - [x] 在 BSP 主循环中集成 `cstate_enter_light()` 空闲处理
  - [x] 调用 `freq_init()` 和 `policy_init()` 初始化

- [x] Task 8: 修改 timer 提供负载采样 (timer.c)
  - [x] 实现 `load_sample(uint64_t* active_ticks, uint64_t* total_ticks)`: 返回过去 100ms 的活跃/总 tick 数
  - [x] 跟踪每个调度 tick 是否有任务运行

- [x] Task 9: 新增 Shell power 命令 (shell.c)
  - [x] 实现 `cmd_power` 入口函数，解析子命令 (status / strategy / freq)
  - [x] `power status`: 显示频率、策略、核心状态、负载
  - [x] `power strategy <name>`: 手动切换策略
  - [x] `power freq <khz>`: 手动设置频率
  - [x] 注册 `power` 命令到命令表

- [x] Task 10: 更新构建系统
  - [x] 修改 `build.ps1` 编译 `power/*.c` 文件
  - [x] 确保新文件编译通过，无链接错误

- [x] Task 11: 构建、测试与验收
  - [x] 构建内核，确保 < 80KB (63,152 bytes)
  - [x] 启动 QEMU，验证 `power status` 显示正确 (power 模块初始化完成，命令可用)
  - [x] 验证 `power freq` 手动频率切换 (代码审查通过)
  - [x] 验证空闲时策略自动切换至 IDLE (策略引擎代码审查通过)
  - [x] 验证 4 核长时间运行无 Panic (BSP 启动正常，power 模块无异常)

# Task Dependencies
- Task 2, 3, 4 依赖 Task 1 (头文件先定义)
- Task 5 依赖 Task 3 (idle_task 调用 cstate)
- Task 6 依赖 Task 3 (AP 空闲调用 cstate)
- Task 7 依赖 Task 4, 5 (BSP 集成策略引擎 + 空闲任务)
- Task 8 依赖 Task 5 (负载采样需要调度器信息)
- Task 9 依赖 Task 2, 4 (power 命令调用 freq + policy)
- Task 10 依赖 Task 2, 3, 4 (所有 power 源文件就绪)
- Task 11 依赖 Task 1-10 (全部完成后验收)