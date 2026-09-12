# W6.5 内核优化实施计划（代码量 < 400 行）

## Summary

在 W6 全量验收 PASS 的基础上，对内核做低成本性能/体积优化。探索核实后，用户 spec 中 8 项优化的**现状与 spec 描述有 3 处重大出入**，已按用户确认的替代方向调整。总目标：全部改动后 `test_w6_e2e.ps1` 仍 13/13 PASS，不破坏现有轮询确定性。

## Current State Analysis

### 探索核实的现状（与 spec 的出入）
| 优化项 | spec 假设 | 实际现状（已核实） | 处置 |
|---|---|---|---|
| #1 syscall O(1) | 线性遍历 40-60 条 | **已实现**：[syscall.c L7](file:///d:/BananaOS-Axion/src/kernel/syscall/syscall.c#L7) `syscall_table[512]` 固定数组，[L85-90](file:///d:/BananaOS-Axion/src/kernel/syscall/syscall.c#L85-L90) `syscall_dispatch` 直接索引，无遍历 | 无需改动，仅验证 |
| #2 启动日志 | 全量输出到 VGA | printk 已有 `PRINTK_CONSOLE_LEVEL`（[printk.c L105](file:///d:/BananaOS-Axion/src/kernel/core/printk.c#L104-L105)）按级别过滤；kmain 的 `[INIT]` 全部是 KERN_INFO | 加 quiet 参数控制启动期级别 |
| #3 pmalloc 热缓存 | 位图扫描 512 字 | 属实：[mm.c L67-80](file:///d:/BananaOS-Axion/src/kernel/mm/mm.c#L67-L80) 线性扫 `hot_bitmap[4096]` | 实现 16 页热缓存 |
| #4 自适应 Tick | APIC Timer 1ms/10ms | **前提不存在**：无 APIC Timer 中断，sched_tick 由主循环轮询调用（[kmain.c L272](file:///d:/BananaOS-Axion/src/kernel/kmain.c#L272)、L300），timer 用 TSC | 改为空载 pause 降频（用户确认） |
| #5 键盘中断 | 门铃轮询 ~100μs | **前提不存在**：无 IRQ1 中断、无 PIC/LAPIC 中断基础设施；键盘是 `while(!kbd_has_key()){pause}` 紧密轮询（[kmain.c L298-322](file:///d:/BananaOS-Axion/src/kernel/kmain.c#L298-L322)） | 键盘轮询提到主循环最前（用户确认） |
| #6 EXT2 目录缓存 | 每次全量读目录 | 属实：[ext2.c L876-887](file:///d:/BananaOS-Axion/src/kernel/fs/ext2.c#L876-L887) `ext2_lookup` 每次 `ext2_read_dir` 读盘 | 实现 64 组 dentry 缓存 |
| #7 栈大小 | 16KB→8KB | **实际 BSP 栈 64KB**：[boot.S L31](file:///d:/BananaOS-Axion/src/boot/boot.S#L31) `STACK_SIZE=0x10000` | 64→16KB（用户确认，省 48KB） |
| #8 死代码 | 未使用函数 | `hotness_update` 未调用（`hotness_age` 被主循环用）；compress 被 boomerang/shell 的 stats 用（活的）；`[dbg]` 输出散布 table.c/shell.c | 条件编译包裹 + 移除 dbg |

### 关键机制确认
- **kmain 签名**：[kmain.c L95](file:///d:/BananaOS-Axion/src/kernel/kmain.c#L95) `kmain(uint32_t magic, void* multiboot_info)`，boot.S L184-185 传入 multiboot_info —— **quiet 参数可从 multiboot2 cmdline tag(type=1) 解析**。
- **MAX_SYSCALLS=512**（[syscall.h L70](file:///d:/BananaOS-Axion/src/kernel/syscall/syscall.h#L70)）。
- **policy 已有 LIGHT 策略**（[policy.c L30-31](file:///d:/BananaOS-Axion/src/kernel/power/policy.c#L30-L31)、L93-94），空载时 `power status` 已可显示 LIGHT。
- **键盘已有 256 环形缓冲**（[keyboard.c L4-15](file:///d:/BananaOS-Axion/src/kernel/core/keyboard.c#L4-L15)），`kbd_has_key()` 内部 `kbd_poll()` 读串口+PS2。
- **约束**：kernel.flat 当前 115KB（<512KB）；启动 ~149ms（<2s）。

## Proposed Changes

### Change 1（#2）：quiet 启动参数控制启动日志（~25 行）
- **文件**：[kmain.c](file:///d:/BananaOS-Axion/src/kernel/kmain.c)（开头部解析）、[printk.h](file:///d:/BananaOS-Axion/src/kernel/core/printk.h)、[printk.c](file:///d:/BananaOS-Axion/src/kernel/core/printk.c)
- **做法**：printk.h 增加 `void printk_set_console_level(int level);`；printk.c 把 `PRINTK_CONSOLE_LEVEL` 改为可变的 `static int g_console_level = KERN_INFO`，`printk()` L105 用它过滤。kmain 起始（kbd/vga 初始化后）解析 multiboot2 cmdline tag（type=1，字符串），若含子串 "quiet" 则 `printk_set_console_level(KERN_WARNING)` —— 跳过全部 `[INIT]`(KERN_INFO) 与 `[dbg]` 的 VGA 输出。
- **注意**：`[BOOT] Boot time` 打印仍保留（KERN_INFO 会因 quiet 被跳过，可改为打印前临时切回或保持 INFO——按最小改动，boot time 保持 INFO 被 quiet 跳过即可，验收以 `Type 'help'` 出现为准）。
- **为什么**：启动 ~150ms 中约 50ms 花在 VGA 输出，quiet 直接跳过。

### Change 2（#3）：pmalloc 16 页热页缓存（~60 行）
- **文件**：[mm.c](file:///d:/BananaOS-Axion/src/kernel/mm/mm.c)
- **做法**：新增 `static uint32_t hot_free_cache[16]; static int hot_cache_len;`。`pmalloc()` 先扫缓存（命中：`hot_bitmap[g]|=1<<bit; hot_zone.used_pages++;` 并清缓存槽），未命中走原位图扫描。`pfree()` 对 hot 区页面：清位图+used_pages 后，若 `hot_cache_len<16` 则存入缓存。warm/boomerang 区不变。
- **正确性**：缓存只存 hot 区页号，pfree 已清位图；pmalloc 从缓存取时重新置位图，保证位图与缓存一致。**必须**保持现有 `hot_zone.used_pages` 计数语义。
- **为什么**：高频小对象（file/inode 等）分配免去 4096 组扫描。

### Change 3（#4）：空载主循环 pause 降频（~30 行）
- **文件**：[kmain.c](file:///d:/BananaOS-Axion/src/kernel/kmain.c)
- **做法**：在键盘等待内层循环 `while (!kbd_has_key())`（L298）中，每轮先 `sched_next()`；若无就绪任务（`next_tid<0`）且当前负载低于阈值，则执行**多次 pause**（如 8 次）代替单次 pause，降低空转频率；有任务或输入时立即恢复单次 pause 保证响应。复用 `sched_load_sample()`。
- **为什么**：无 APIC Timer 前提下，用 pause 计数控制空转节奏，空载时省电（TCG 下体现为 CPU 占用下降），不引入中断基础设施。

### Change 4（#5）：键盘轮询移至主循环最前（~10 行）
- **文件**：[kmain.c](file:///d:/BananaOS-Axion/src/kernel/kmain.c)
- **做法**：把 `kbd_has_key()`/输入检查提到主循环顶部（sched_tick 之前），输入优先处理；调度放其后。保证打字响应不被调度 tick 延迟。
- **为什么**：键盘已是紧密轮询，此项是用户要求的最小调整；不建中断。

### Change 5（#6）：EXT2 64 组 dentry 缓存（~80 行）
- **文件**：[ext2.c](file:///d:/BananaOS-Axion/src/kernel/fs/ext2.c)
- **做法**：
  - 新增静态表 `typedef struct { uint32_t parent; uint32_t hash; uint32_t child; uint8_t valid; } dentry_t; static dentry_t dcache[64];` + `static uint32_t dcache_next;`（循环替换，简化 LRU）。
  - `ext2_lookup()`（L876）先线性扫 dcache 匹配 (parent, hash)；命中直接返回 child；未命中走原 `ext2_read_dir`，成功后插入 dcache。
  - **失效**：`ext2_unlink`/`ext2_mkdir`/`ext2_create_file` 所在目录路径变更后清空整表（`dcache` 全置 invalid）——文件系统操作低频，全清可接受，保证正确性。
  - 提供 `static uint32_t name_hash(const char* s)`（如 FNV-1a 简化版）。
- **为什么**：`ls /bin` 等对同一目录多次 lookup，缓存避免重复读盘。

### Change 6（#7）：BSP 内核栈 64KB→16KB（~2 行）
- **文件**：[boot.S L31](file:///d:/BananaOS-Axion/src/boot/boot.S#L31)
- **做法**：`.set STACK_SIZE, 0x10000` → `0x4000`。
- **风险**：BSP 栈在 long_mode_start 与 kmain 早期使用（含 FPU 初始化、函数调用）。16KB 足够（现有代码最大栈帧远小于此；Shell 是状态机非线程）。**不改** ksyscall_stack(16KB)/wait4_kern_stack(8KB)/AP trampoline。
- **为什么**：省 48KB .bss → kernel.flat 从 115KB 降至 ~70KB。

### Change 7（#8）：条件编译包裹 + 移除 dbg 输出（~20 行）
- **文件**：`src/kernel/mm/hotness.c`、`src/kernel/mm/boomerang.c`、`src/kernel/syscall/table.c`（[dbg] fork/execve）、`src/kernel/core/shell.c`（如有 [dbg]）、`src/kernel/syscall/shm.c`（shm_dbg）
- **做法**：
  - `hotness_update()` 定义外裹 `#if 0 ... #endif`（未被调用）；`hotness_age()` 保留（主循环在用）。
  - boomerang 的交换分配路径若未被任何命令调用，整体 `#if 0` 包裹，保留 `compress.c`（`compress_ratio` 被 shell stats 使用，活的）。
  - 移除 `[dbg] fork pid=...`、`[dbg] execve ...`、`[dbg] shmget id=...` 等调试输出（替换为空或直接删除）。
- **为什么**：减小体积 + 清理串口噪音；**不破坏 stats/compress 命令功能**。

### Change 8（#1 syscall）：仅验证，不改
- [syscall.c](file:///d:/BananaOS-Axion/src/kernel/syscall/syscall.c) 已是固定数组 O(1)（512 槽）。spec 要求的"256 槽"与现有 512 槽等价，无需改动。**验收时确认无回归**。

## Assumptions & Decisions

- **不引入中断基础设施**（无 PIC/LAPIC timer、无 IRQ1）。#4 用 pause 降频、#5 用轮询提前，均为用户确认的替代方案。
- **#7 按实际值**：BSP 栈 64→16KB（用户明确选择），非 spec 的 16→8KB。
- **dentry 缓存失效**：写操作（unlink/mkdir/create）后清空全表，正确性优先。
- **quiet 参数**：QEMU 命令行经 multiboot2 cmdline tag 传入；无此参数时行为与现在完全一致（默认 KERN_INFO）。
- 所有优化保持 W6 轮询确定性：**不改变 syscall 语义、调度语义、tty 回显行为**。

## Verification

1. 构建：`powershell -ExecutionPolicy Bypass -File .\build.ps1` 通过；`kernel.flat` 记录体积（预期 ~70KB，因栈 64→16KB）。
2. 全量回归：`powershell -ExecutionPolicy Bypass -File .\test_w6_e2e.ps1` → **13/13 PASS**（perm/sig/shm/tcp/big/proc/sys/ps/bash/文件持久化/jobctl 全过）。
3. 回归关键路径：bash echo、外部命令（fork/execve/wait4）、快速打字即时回显、job control。
4. 体积监控：`ls -l kernel.flat` < 110KB（预期 ~70KB）。
5. 启动验证：默认启动 `Type 'help'` 出现；`power status` 空载显示 LIGHT（policy 已有，确认未回归）。
6. 若某项回归：按 Change 粒度回滚该项并重测。

## 实施顺序

1. Change 6（栈，最简单、独立）→ 构建验证体积
2. Change 1（quiet）+ Change 4（键盘前置，改动小）
3. Change 2（pmalloc 缓存）+ Change 3（pause 降频）
4. Change 5（dentry 缓存，最大）
5. Change 7（死代码/dbg 清理）
6. 每批后跑 build + test_w6_e2e.ps1；最后统一跑全量回归
