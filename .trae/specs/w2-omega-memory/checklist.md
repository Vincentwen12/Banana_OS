# Checklist: W2 — Ω弹性内存（完整版）

## 目录结构
- [x] `src/kernel/mm/` 目录存在
- [x] `src/kernel/mm/mm.c` 和 `mm.h` 就位
- [x] `src/kernel/mm/hotness.c` 和 `hotness.h` 就位
- [x] `src/kernel/mm/compress.c` 和 `compress.h` 就位
- [x] `src/kernel/mm/boomerang.c` 和 `boomerang.h` 就位
- [x] 旧文件 `src/kernel/mm.c`, `src/kernel/mm.h` 已删除
- [x] 旧文件 `src/kernel/hotness.c`, `src/kernel/hotness.h` 已删除

## axion.h 常量
- [x] TOTAL_MEMORY = 0x80000000 (2GB)
- [x] HEAP_BASE = 0x800000 (8MB, 实际值因BSS对齐调整)
- [x] WARM_BASE = HEAP_BASE + HOT_ZONE_SIZE = 0x40800000
- [x] BOOMERANG_BASE = WARM_BASE + WARM_ZONE_SIZE = 0x60800000
- [x] HOT_ZONE_PAGES = 262144 (1024MB / 4KB)
- [x] WARM_ZONE_PAGES = 131072 (512MB / 4KB)
- [x] BOOMERANG_PAGES = 65536 (256MB / 4KB)
- [x] NUM_PAGE_GROUPS = HOT_ZONE_PAGES / 64 = 4096

## mm.c 分区分配器
- [x] `zone_t` 结构体定义完整（base_addr, total_pages, used_pages, bitmap, bitmap_groups, compressed_equiv, evict_count）
- [x] `mm_init()` 正确初始化三个 zone
- [x] `pmalloc()` 从热区分配
- [x] `pfree()` 根据地址判断所属 zone
- [x] `mm_stats()` 返回三级分区统计
- [x] `mm_zone_stats()` 实现
- [x] `mm_stats_full()` 实现

## hotness 热度引擎
- [x] `hotness_get(page_idx)` 实现
- [x] `hotness_reset(page_idx)` 实现
- [x] `hotness_update()` 实现 (4-bit 打包)
- [x] `hotness_age()` 实现 (右移1位半衰)
- [x] 10ms 老化周期在 kmain 中实现 (timer_ms 驱动)

## compress XOR 压缩
- [x] `compress_init()` 实现
- [x] `compress_set_base()` 实现
- [x] `compress_xor()` 实现 XOR + 末尾零字节截断
- [x] `decompress_xor()` 实现还原
- [x] `compress_ratio()` 返回累积压缩比
- [x] `compress_get_stats()` 实现

## boomerang 回旋镖池
- [x] `boomerang_init()` 实现
- [x] `boomerang_evict_cold()` 实现热区→温区/回旋镖池迁移
- [x] 温区满时二次压缩到回旋镖池
- [x] `boomerang_restore()` 实现解压回迁
- [x] `boomerang_stats()` 返回使用量与等效容量
- [x] `boomerang_manual_compress()` 实现

## Shell 命令
- [x] `mem` 显示三级分区布局
- [x] `stats` 显示使用率与压缩比
- [x] `compress <addr>` 手动压缩
- [x] `alloc` 不足时调用 boomerang_evict_cold，最终显示 "OOM: hot zone exhausted"
- [x] `help` 包含 stats 和 compress 命令

## 构建系统
- [x] `build.ps1` 编译 `src/kernel/mm/*.c`
- [x] `build.ps1` 包含 `-Isrc/kernel/mm` 路径
- [x] `build.ps1` QEMU `-m 2G`
- [x] `Invoke-Clean` 清理 mm/ 产物
- [x] `kmain.c` include 路径正确

## 启动验证
- [x] `.\build.ps1 all` 编译无错误 (kernel.bin 22.4KB, kernel.flat 12.3KB)
- [x] `.\build.ps1 run` 启动到 Shell 提示符 `>`
- [x] Boot time: ~4-5ms (< 2s target)
- [x] 所有模块初始化成功 (timer, mm, hotness, sched, doorbell, compress, boomerang, keyboard, shell)

## 修复的 Bug
- [x] BSS段清零破坏页表: 将PML4/PDPT移入独立.pgtable段
- [x] PDPT循环写入2048条记录(超出512条限制): 拆分为PDPT0+PDPT1各512条
- [x] 页表映射不足(仅1GB): 扩展为2GB映射(PML4[0]+PML4[1])
- [x] 内核BSS段6.1MB: HEAP_BASE调整为0x800000 (8MB)避免重叠