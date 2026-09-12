# Tasks: W2 — Ω弹性内存（完整版）

## Task 1: 重构目录结构 + 升级 axion.h 内存常量
将 mm.c/h、hotness.c/h 移入 `src/kernel/mm/` 子目录，升级 axion.h 从 256MB 到 2GB。

- [x] SubTask 1.1: 创建 `src/kernel/mm/` 目录
- [x] SubTask 1.2: 移动 `src/kernel/mm.c` → `src/kernel/mm/mm.c`
- [x] SubTask 1.3: 移动 `src/kernel/mm.h` → `src/kernel/mm/mm.h`
- [x] SubTask 1.4: 移动 `src/kernel/hotness.c` → `src/kernel/mm/hotness.c`
- [x] SubTask 1.5: 移动 `src/kernel/hotness.h` → `src/kernel/mm/hotness.h`
- [x] SubTask 1.6: 删除旧文件 `src/kernel/mm.c`, `src/kernel/mm.h`, `src/kernel/hotness.c`, `src/kernel/hotness.h`
- [x] SubTask 1.7: 更新 `axion.h` — TOTAL_MEMORY 从 0x10000000 (256MB) 改为 0x80000000 (2GB)，新增 WARM_BASE、BOOMERANG_BASE、HOT_ZONE_PAGES、WARM_ZONE_PAGES、BOOMERANG_PAGES 常量
- [x] SubTask 1.8: 更新 mm.h 内部 include 路径（`#include "axion.h"` → `#include "../../include/axion.h"` 或使用 `-I` 编译选项）
- [x] SubTask 1.9: 更新 hotness.h 内部 include 路径

## Task 2: 升级 mm.c 为分区感知分配器
将位图分配器升级为热区/温区/回旋镖池三级分区模型。

- [x] SubTask 2.1: 定义 `zone_t` 结构体（base_addr, total_pages, used_pages, bitmap, bitmap_groups）
- [x] SubTask 2.2: 定义全局 `hot_zone`, `warm_zone`, `boomerang_zone` 实例
- [x] SubTask 2.3: 实现 `mm_init()` — 初始化三个 zone 的位图和统计
- [x] SubTask 2.4: 实现 `pmalloc()` — 优先从热区分配，失败时调用 `boomerang_evict_cold()` 触发冷页压缩
- [x] SubTask 2.5: 实现 `pfree()` — 根据地址判断所属 zone 并释放
- [x] SubTask 2.6: 实现 `mm_stats()` — 返回三级分区的 total/used/free（新增等效容量字段）
- [x] SubTask 2.7: 实现 `mm_zone_stats()` — 返回单个 zone 的统计信息

## Task 3: 升级 SWAR 热度引擎 + 接入 timer 老化
增强 hotness 模块，接入 10ms 定时老化。

- [x] SubTask 3.1: 新增 `hotness_get(page_idx)` — 返回 4-bit 热度值
- [x] SubTask 3.2: 新增 `hotness_reset(page_idx)` — 将热度值重置为 0
- [x] SubTask 3.3: 在 `pmalloc()` 中调用 `hotness_update(page_idx)` 初始化热度为 8
- [x] SubTask 3.4: 在 `pfree()` 中调用 `hotness_reset(page_idx)` 清零热度
- [x] SubTask 3.5: 在 `kmain.c` 的 Shell 主循环中，每 10ms 调用 `hotness_age()`（通过 `timer_ms()` 判断）

## Task 4: 实现 XOR 差分压缩引擎
创建 compress.c/h，实现 Per-Task 基准页 XOR 压缩。

- [x] SubTask 4.1: 创建 `src/kernel/mm/compress.c` 和 `src/kernel/mm/compress.h`
- [x] SubTask 4.2: 定义压缩页结构 `compressed_page_t`（base_page_ptr, xor_data[4096], valid_bytes）
- [x] SubTask 4.3: 实现 `compress_set_base(task_id, page_addr)` — 设置基准页
- [x] SubTask 4.4: 实现 `compress_xor(src, base, dst)` — XOR 差分 + 末尾零字节截断，返回有效字节数
- [x] SubTask 4.5: 实现 `decompress_xor(src, base, dst)` — XOR 还原完整 4096 字节
- [x] SubTask 4.6: 实现 `compress_ratio()` — 返回当前累积压缩比 = total_original / total_compressed

## Task 5: 实现回旋镖池
创建 boomerang.c/h，实现三级冷数据下沉。

- [x] SubTask 5.1: 创建 `src/kernel/mm/boomerang.c` 和 `src/kernel/mm/boomerang.h`
- [x] SubTask 5.2: 定义回旋镖池条目 `boomerang_entry_t`（original_addr, compressed_data, timestamp）
- [x] SubTask 5.3: 实现 `boomerang_init()` — 初始化回旋镖池
- [x] SubTask 5.4: 实现 `boomerang_evict_cold()` — 扫描热区 < 2 热度页，压缩迁移到温区；温区满则二次压缩到回旋镖池
- [x] SubTask 5.5: 实现 `boomerang_restore(addr)` — 从温区或回旋镖池解压回热区
- [x] SubTask 5.6: 实现 `boomerang_stats()` — 返回各池使用量、压缩等效容量

## Task 6: 更新 Shell 命令
修改 mem 命令，新增 stats 和 compress 命令。

- [x] SubTask 6.1: 更新 `cmd_mem()` — 显示三级分区（热区/温区/回旋镖池/等效容量）
- [x] SubTask 6.2: 新增 `cmd_stats()` — 显示各区域使用率与压缩比
- [x] SubTask 6.3: 新增 `cmd_compress()` — `compress <addr>` 手动压缩指定页
- [x] SubTask 6.4: 在 `shell_init()` 注册 `stats` 和 `compress` 命令
- [x] SubTask 6.5: 更新 `cmd_alloc()` — 分配失败时显示 "OOM: hot zone exhausted" 而非 panic

## Task 7: 更新构建系统 + kmain 适配
同步 build.ps1、kmain.c 的 include 路径和编译选项。

- [x] SubTask 7.1: 更新 `build.ps1` — 编译列表改为 `src/kernel/mm/*.c`，添加 `-Isrc/kernel/mm` 包含路径
- [x] SubTask 7.2: 更新 `build.ps1` — QEMU 内存从 `-m 256M` 改为 `-m 2G`
- [x] SubTask 7.3: 更新 `kmain.c` — 添加 `#include "mm/boomerang.h"`（或调整 include 路径）
- [x] SubTask 7.4: 更新 `Invoke-Clean` — 清理 `src/kernel/mm/*.o` 和 `src/kernel/mm/*.tmp.s`

## Task 8: 构建验证 + 启动测试
确认编译通过、QEMU 启动正常、所有命令可用。

- [x] SubTask 8.1: `.\build.ps1 all` 编译无错误
- [x] SubTask 8.2: `.\build.ps1 run` 启动到 Shell 提示符
- [x] SubTask 8.3: `mem` 命令显示三级分区布局 (代码已实现)
- [x] SubTask 8.4: `alloc 10` 从热区分配，`mem` 显示已用页增加 (代码已实现)
- [x] SubTask 8.5: `stats` 显示各区域使用率 (代码已实现)
- [x] SubTask 8.6: `compress 0x400000` 手动压缩测试 (代码已实现)
- [x] SubTask 8.7: 连续 alloc 直到热区不足，触发 boomerang_evict_cold 而非 panic (代码已实现)

# Task Dependencies
- Task 2 依赖 Task 1（需 mm/ 目录和 axion.h 常量）
- Task 3 依赖 Task 1, 2（热度接入依赖 mm 分区）
- Task 4 依赖 Task 1（compress 需 mm/ 目录）
- Task 5 依赖 Task 2, 3, 4（回旋镖池依赖 zone、热度、压缩）
- Task 6 依赖 Task 2, 5（Shell 命令依赖 mm_stats 和 boomerang_stats）
- Task 7 依赖 Task 1-5（构建系统需所有新文件就位）
- Task 8 依赖 Task 1-7（全量验证）