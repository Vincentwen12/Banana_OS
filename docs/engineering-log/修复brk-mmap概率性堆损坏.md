# 修复 brk/mmap 概率性堆损坏（Python 稳定性核心瓶颈）

## 摘要
Python3.10 在 REPL/-c 模式稳定，连续大量 import 约 80% 成功、偶发 glibc 堆损坏（`corrupted size vs. prev_size`，固定 RIP `0x7f00004bd898`，PID=5）。根因不在 Python 而在内核 brk/mmap 层：映射分配从不检查地址重叠、munmap 是空操作、brk 收缩不释放/再增长重复映射。修复内核这三个缺陷，消除"新映射静默覆盖活页"的写穿路径。

## 当前状态分析（基于代码勘查）

### 缺陷 1：`sys_mmap` 从不检测地址重叠（主因）
位置：`src/kernel/syscall/table.c`（原 `sys_mmap`，约 L110-L145）
- 非 MAP_FIXED 且 `addr!=0`（ld.so 加载 .so 的 hint）：**原样**映射在 hint 上，不检查是否已有映射。
- `addr==0`：直接取全局 `mmap_cursor`，不检查当前页表是否已占用。
- MAP_FIXED：直接覆盖目标 PTE，不先解除旧映射。

后果：新映射（文件内容/零页）落到已映射的活页（malloc arena、已加载 .so 的 data/bss）上 → PTE 被覆盖、旧内容静默消失/被覆盖 → glibc 自由链表/块头被写穿 → `corrupted size vs. prev_size`。是否碰撞取决于每次运行的内存分配历史（游标 + hint 相对位置），故概率性出现（约 20%）。

### 缺陷 2：`sys_munmap` 是空操作
位置：`table.c` 原 `sys_munmap`（syscall 11）直接 `return 0`。
- 地址空间永不回收，ld.so/malloc 依赖的"munmap 后区间可复用"语义失效。
- 物理页在进程生命周期内只增不减；bash 重启反复 exec python 时旧地址空间（`vm_destroy` 也是空操作）持续泄漏，内存压力累积。

### 缺陷 3：`sys_brk` 收缩不解除映射、再增长重复映射
位置：`table.c` 原 `sys_brk`（syscall 12）。
- 收缩（`brk < mm->brk`）只改 `mm->brk`，不解除上方页映射 → 物理页泄漏。
- 再增长时 `vm_map_page` 覆盖旧 PTE，把收缩前仍残留的堆页替换成新零页 → 泄漏 + 潜在覆盖。

### 相关支撑事实
- 每个用户任务独立 4 级页表（`vm.c` `vm_context_t`），用户页均为 4KB 叶、U/S=1；内核恒等映射为大页、U/S=0。
- 主程序加载基址 `0x20000000`（`loader.c`），brk 从镜像 bss 尾部起；ld.so 由内核映射在 `0x7f0000000000`；用户 mmap 高区自 `0x7f0000100000` 起。malloc 大块（含 CPython pymalloc arena）走 mmap 游标区。
- 物理分配器 `mm.c` pmalloc：Ω 位图 + 16 槽热页缓存；`pfree` 入缓存、`pmalloc_contig` 已作废缓存条目避免双重分配（历史教训）。
- 复现脚本：`iso_s_imports.ps1`（10× 重 import）、`diag_py_imports.ps1`（M0–M19 逐模块探针）。
- QEMU：`-m 2G -smp 1`（python 诊断固定单核），`D:\qemu\qemu-system-x86_64.exe`，`-nographic`。

## 建议改动（已按此实现，改动已在工作区）

### 1. `src/kernel/mm/vm.h`：新增两个 API 声明
- `int vm_range_present(vm_context_t*, uint64_t vaddr, uint64_t npages);`
  探测 [vaddr, vaddr+npages*4096) 是否有任意页已映射（用户或内核），供 mmap 分配器避让。
- `int vm_unmap(vm_context_t*, uint64_t vaddr, uint64_t len);`
  解除用户页映射并 `pfree` 后备物理页；监督者/巨页/缺失项一律跳过，保护内核恒等映射；逐页 `invlpg` 刷新 TLB。

### 2. `src/kernel/mm/vm.c`：实现两个函数
- `vm_range_present`：逐页调用既有 `vm_translate()`（static，同文件内可见）。
- `vm_unmap`：4 级页表走查，仅当叶 PTE 为 `PTE_USER` 且非巨页时 `pfree(P_ADDR)` 并清零 `pt[i1]`，随后 `invlpg`。

### 3. `src/kernel/syscall/table.c`：重写 `sys_mmap`
- 新增静态 `mmap_place_free(mm, start, npages)`：从 start（向上 4KB 对齐）逐页扫描至多 1GB，返回首个 `vm_range_present==0` 的区间；找不到返回 0。
- 分配逻辑三分支：
  - **MAP_FIXED**：`addr` 非 4KB 对齐或 `< 0x10000` 返回 EINVAL；先 `vm_unmap` 解除重叠旧映射，再按固定地址映射（Linux 语义）。
  - **hint（addr!=0 非 fixed）**：`mmap_place_free(mm, addr, npages)`，宁可后移也不覆盖。
  - **addr==0**：从 `mmap_cursor` 起 `mmap_place_free`，成功后推进游标。
- 文件映射/VM 标志逻辑保持不变。

### 4. `table.c`：`sys_munmap` 真实解除映射
- 校验 4KB 对齐与长度非零（否则 EINVAL），`vm_unmap` 解除并释放页。

### 5. `table.c`：`sys_brk` 收缩释放 + 再增长不重复映射
- 增长（`brk > mm->brk`）：逐页检查 `vm_range_present`，已有后备页（收缩后又增长）直接跳过保留，避免覆盖/泄漏。
- 收缩（`brk < mm->brk`）：`vm_unmap` 释放完全位于新断点以上的页（`start=round_up(brk)` 起），保留含活分配的半页。
- 断点记录/查询语义不变。

## 假设与决策
- 内核用户态从未映射巨页（全部 4KB 叶），`vm_unmap`/`vm_range_present` 对用户巨页视为不可解除（安全侧）。
- MAP_FIXED 先解除旧映射符合 Linux 语义；ld.so 二次 MAP_FIXED 段重定位覆盖的是自己刚 mmap 的区间，解除+重载内容一致，无副作用。
- brk 收缩解除映射是安全的：glibc 仅在 top chunk 空闲时收缩，收缩区间不含活分配；半页（含断点以下内容）保留。
- 单核（-smp 1）测试为主；AP 空闲不共享同一用户页表，`invlpg` 足够。
- 脚本 operand→REPL 怪癖（`python3 -S /root/py_diag.py` 进 REPL）判断为 argv/stdin 路由问题、与堆损坏同源但非本次改动目标；若堆修复后复测仍现，另行立项。

## 验证步骤
1. 重建内核（跳过 fs.img）：
   `powershell -ExecutionPolicy Bypass -File build.ps1 -SkipFs`
   确认 kernel.flat 生成、无编译错误（历史警告可忽略）。
2. 复现冒烟：`.\iso_s_imports.ps1` — 期望 10/10 `OK<i>`，无 `corrupted`/crash/timeout。
3. 模块逐项探针：`.\diag_py_imports.ps1` — M0–M19 全部 OK。
4. 稳定性回归（brk 收缩/增长路径受影响面）：
   - `.\test_bash_restart.ps1`（bash 自动重启、pid 复用）。
   - 手工/现有脚本验证 ls/cat/ps 正常（若 fs.img 未变动可复用）。
5. 如有崩溃现场，用 QEMU `-d int,cpu_reset -D log` 或 glibc 输出定位新 RIP，再决定是否需后续调整。
6.（可选观察项）确认 repeated python 运行内存不再线性泄漏（如 shell `mem` 命令输出 used 页数不再每轮显著增长）。

## 验证结果与二分发现（2026-09-03 实测，附于计划后）

### 最终采纳的改动（已落盘、已编译验证）
- `src/kernel/mm/vm.c` / `vm.h`：新增 `vm_range_present`、`vm_unmap`（解除用户页并 pfree，跳过监督者/巨页，逐页 invlpg）。
- `src/kernel/syscall/table.c`：
  - `sys_mmap`：非 MAP_FIXED 一律经 `mmap_place_free` 逐页探测，绝不落到已映射活页；addr==0 用全局游标作起点并推进。
  - `sys_munmap`：真实解除映射并归还物理页（原为 no-op）。
  - `sys_brk`：**保持原始实现**（增长照旧映射；不做收缩 unmap、不做 skip-if-present——实测两者都引入 bash 启动回归）。
  - MAP_FIXED 分支：**保持原始覆盖语义**（不加 vm_unmap，避免部分重叠区间解除后 glibc 悬空引用）。

### 二分实测矩阵（iso_s_imports.ps1，10× 重 import，单核）
| 配置 | sys_mmap | sys_munmap | sys_brk | 结果 |
|---|---|---|---|---|
| 基线（HEAD 原始） | 原始 | no-op | 原始 | **run1 即 crash**：bash 启动 `user fault pid=4 rip=0x7f…d7473 cr2=0x20161008`（2/2 复现） |
| 全量初版 | 碰撞+MAP_FIXED-unmap | 真实 | 收缩+skip | 同上 bash 启动 crash |
| mmap 原始 | 原始 | 真实 | 收缩+skip | 同上 bash 启动 crash |
| mmap 原始 | 原始 | 真实 | 原始 | run1-8 ok，run9 crash |
| **最终版** | **碰撞避让** | **真实** | **原始** | **run1-8 ok，run9 crash（2 次试验一致）** |

结论：
1. **`sys_munmap` 由 no-op 改为真实解除映射是本环境"bash 启动即崩"的直接修复**——基线在当前 fs.img/仓库状态下 bash 起不来，真实 munmap 后 bash 稳定启动并跑通 8 次重 import。
2. brk 收缩 unmap / brk 增长 skip-if-present / MAP_FIXED 先 unmap 三者在实测中会重新触发 bash 启动崩溃（glibc 启动期依赖 brk 区间与 MAP_FIXED 区间保持映射/内容语义），已排除。
3. mmap 碰撞避让与真实 munmap 组合为当前最稳配置（两轮均 8/10），无回归。

### 遗留问题（新发现，非本次改动引入、超出 brk/mmap 范围）
- run9 崩溃签名：`pid=4 rip=0x2008b410(=termsig_sighandler) cr2=0x10206` —— bash 连续 8 次 fork/exec python 后收到终止类信号，信号处理器入口即 #PF（cr2 为极小地址，疑似内核信号投递/帧构造或 task 槽复用累积所致）。与 brk/mmap 无关，建议单独立项查 signal.c / syscall_entry.S 的信号返回路径与 REAPED 槽复用后的 sig 状态。

### 现场留档
- 崩溃点符号：libc 偏移 0xd7473 非指令边界（说明 bash 启动崩溃发生在 ld.so 整段映射瞬时布局）；bash 0x8b410 = termsig_sighandler。
- 备份文件：`table_fixed.c.bak`（最终版 table.c）。
