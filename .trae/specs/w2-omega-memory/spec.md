# W2 — Ω弹性内存（完整版） Spec

## Why
当前 W1 内存管理仅实现了基础位图分配器，256MB 线性堆无压缩、无分层、无热度追踪的实际应用。W2 升级为 2GB 三级弹性内存，通过 SWAR 热度引擎 + XOR 差分压缩 + 回旋镖池，使 2GB 物理内存呈现 ~2.4GB 等效容量。

## What Changes
- **BREAKING**: 内存模型从 256MB 线性堆重构为 2GB 三级分区（热区/温区/回旋镖池）
- **BREAKING**: mm.c/mm.h 和 hotness.c/h 移入 `src/kernel/mm/` 子目录
- 新增 `src/kernel/mm/compress.c/h` — XOR 差分压缩引擎
- 新增 `src/kernel/mm/boomerang.c/h` — 三级回旋镖池
- 更新 `axion.h` — 内存常量从 256MB 升级到 2GB，新增分区地址常量
- 更新 `shell.c` — mem 命令显示三级分区，新增 stats/compress 命令
- 更新 `build.ps1` — 新增 mm/ 子目录编译，添加 `-Isrc/kernel/mm` 包含路径
- 更新 `kmain.c` — 包含路径适配

## Impact
- Affected specs: axion-ban-v6.1 (W2 部分)
- Affected code:
  - `src/include/axion.h` — 内存常量升级
  - `src/kernel/mm/mm.c` — 从 kernel/ 移入，升级为分区感知分配器
  - `src/kernel/mm/mm.h` — 新增 zone 统计接口
  - `src/kernel/mm/hotness.c` — 从 kernel/ 移入，接入 timer 老化
  - `src/kernel/mm/hotness.h` — 新增 hotness_get() 查询接口
  - `src/kernel/mm/compress.c` — **新增** XOR 压缩引擎
  - `src/kernel/mm/compress.h` — **新增**
  - `src/kernel/mm/boomerang.c` — **新增** 回旋镖池
  - `src/kernel/mm/boomerang.h` — **新增**
  - `src/kernel/shell.c` — mem/stats/compress 命令
  - `src/kernel/kmain.c` — include 路径更新
  - `build.ps1` — 编译列表 + include 路径

## ADDED Requirements

### Requirement: 2GB 三级内存分区
系统 SHALL 将 2GB 物理内存划分为热区(1024MB)、温区(512MB)、回旋镖池(256MB)，其余为保留区。

#### Scenario: 内存布局
- **WHEN** 系统启动
- **THEN** 热区起始于 HEAP_BASE (0x400000)，大小 1024MB
- **AND** 温区起始于 WARM_BASE (0x40400000)，大小 512MB
- **AND** 回旋镖池起始于 BOOMERANG_BASE (0x60400000)，大小 256MB

#### Scenario: mem 命令显示三级分区
- **WHEN** 用户执行 `mem`
- **THEN** 输出格式：
  ```
  物理内存: 2048MB
  热区: 1024MB (已用 XXX MB)
  温区: 512MB (压缩等效 1280MB)
  回旋镖池: 256MB (压缩等效 640MB)
  等效总容量: ~2432MB
  ```

### Requirement: SWAR 热度引擎接入 timer
系统 SHALL 每 10ms 触发一次全局热度老化，通过 `hotness_age()` 将每个页的 4-bit 热度值右移 1 位（半衰）。

#### Scenario: 10ms 老化周期
- **WHEN** 系统运行超过 10ms
- **THEN** `hotness_age()` 被调用，所有页热度值减半
- **AND** 热度值 < 2 的页被标记为"冷页"候选

#### Scenario: 热度查询
- **WHEN** 调用 `hotness_get(page_idx)`
- **THEN** 返回该页 4-bit 热度值 (0-15)

#### Scenario: 热度接入分配
- **WHEN** 调用 `pmalloc()`
- **THEN** 分配页的热度值初始化为 8（中等热度）
- **AND** 调用 `hotness_update(page_idx)` 记录

#### Scenario: 热度接入释放
- **WHEN** 调用 `pfree(addr)`
- **THEN** 对应页的热度值重置为 0

### Requirement: XOR 差分压缩引擎
系统 SHALL 提供 Per-Task 基准页 XOR 差分压缩，压缩比 2.5~3.0x。

#### Scenario: 设置基准页
- **WHEN** 调用 `compress_set_base(task_id, page_addr)`
- **THEN** 该任务的基准页指针被保存

#### Scenario: Xor差分压缩
- **WHEN** 调用 `compress_xor(src_page, base_page, dst_buf)`
- **THEN** 对 src_page 和 base_page 逐字节 XOR
- **AND** 结果存入 dst_buf（4096 字节）
- **AND** 返回压缩后有效字节数（run-length 编码）

#### Scenario: Xor解压
- **WHEN** 调用 `decompress_xor(compressed_buf, base_page, dst_page)`
- **THEN** 对 compressed_buf 和 base_page 逐字节 XOR
- **AND** 结果写入 dst_page（4096 字节）

#### Scenario: compress 命令
- **WHEN** 用户执行 `compress 0x400000`
- **THEN** 对该地址页执行 XOR 压缩
- **AND** 输出压缩前后大小和压缩比

### Requirement: 回旋镖池三级下沉
系统 SHALL 实现冷页从热区→温区→回旋镖池的自动迁移。

#### Scenario: 热区不足触发冷页压缩
- **WHEN** `pmalloc()` 在热区位图中找不到空闲页
- **THEN** 扫描所有热区页，将热度 < 2 的冷页压缩迁移到温区
- **AND** 释放热区页供分配

#### Scenario: 温区不足触发二次压缩
- **WHEN** 温区满且需要接收新冷页
- **THEN** 扫描温区页，将最冷的页二次压缩迁移到回旋镖池
- **AND** 释放温区槽位

#### Scenario: 回旋镖池满
- **WHEN** 回旋镖池满且需要接收新页
- **THEN** 丢弃最旧的压缩页（LRU 淘汰）
- **AND** 输出警告 "Boomerang pool full, discarding oldest page"

### Requirement: stats 命令
系统 SHALL 提供 `stats` 命令显示各区域使用率与压缩比。

#### Scenario: stats 输出
- **WHEN** 用户执行 `stats`
- **THEN** 输出：
  ```
  === Memory Zone Statistics ===
  热区 (Hot):    1024MB total,  XXX MB used,  XXX% usage
  温区 (Warm):   512MB total,  XXX pages,  compressed equiv: XXX MB,  ratio: 2.5x
  回旋镖池 (Boom): 256MB total,  XXX pages,  compressed equiv: XXX MB,  ratio: 2.5x
  ----------------------------------------
  等效总容量: ~XXX MB  (物理 2048MB)
  ```

### Requirement: alloc 自动触发冷页压缩
系统 SHALL 在 `alloc` 命令分配失败时自动触发冷页压缩而非直接 panic。

#### Scenario: alloc 热区不足
- **WHEN** 用户执行 `alloc 100` 但热区空闲页不足
- **THEN** 自动触发 boomerang_evict_cold() 压缩冷页
- **AND** 继续分配，若仍不足则输出 "OOM: hot zone exhausted"

## MODIFIED Requirements

### Requirement: pmalloc 改为分区感知
原 W1 `pmalloc()` 从全局位图分配，现改为：
- 优先从热区分配
- 热区不足时触发冷页压缩
- 仍不足时从温区或回旋镖池回迁

#### Scenario: 正常分配
- **WHEN** 热区有空闲页
- **THEN** 直接从热区位图分配，更新 `hot_zone.used`

#### Scenario: 热区不足
- **WHEN** 热区位图无空闲页
- **THEN** 调用 `boomerang_evict_cold()` 迁移冷页
- **AND** 在释放的热区页上分配

### Requirement: mem 命令显示升级
原 `mem` 命令显示简单的 total/used/free，现改为显示三级分区详情。

## REMOVED Requirements
无