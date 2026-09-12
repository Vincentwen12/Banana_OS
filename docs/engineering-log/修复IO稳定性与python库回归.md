# 修复 I/O 稳定性：串口输入丢字节 + python 读盘错位（阶段B收尾）

## 摘要

承接《工具链移植与bash体验增强.md》阶段 B。诊断已证明：**python 库并非缺失**（ssl/hashlib/sqlite3/ctypes/decimal/bz2/lzma/crypt/mmap 短命令实测 import 全 OK），用户感知的"python 有些库不支持"实为两个内核 I/O bug 造成的假象：

1. **串口输入丢字节**（确定复现）：向 bash 一次注入 >~45 字符的命令行，echo 在第 44 字符处停止、命令不完整（日志现场：`...urllib.request,u` 截断；另一次 `_collections_abc.py` 变 `_colltions_abc.py` 丢 2 字节）。host 分批慢发（8B/25ms）仍停在同一字符位 → **不是 QEMU FIFO 突发丢，而是内核接收/调度路径在某点停止排空**。
2. **python 批量 import 读 .py 内容错位**（曾复现：`_collections_abc.py` 同一文件不同 boot 在 line 794/909 报 `MutableSet`/`ItemsView` 缺失；而 `wc -c/-l` 读同一文件正确）→ 磁盘 PIO 读偶发返回错位/脏数据，已加 `ata_pio_read_verified`（双读校验）**尚未验证**。

另：已确认 `python3 -S` 稳定跑通 `import os,ssl,sqlite3,_crypt,ctypes`，证明 python 二进制与 fs 本身健康。

## 现状分析（基于已探明的代码）

### 输入链路
- host → QEMU 16550（FIFO 16B，`vga.c` 已写 FCR=0xC7 使能）→ 内核**仅轮询**读取：[keyboard.c](file:///d:\BananaOS-Axion\src\kernel\core\keyboard.c) `kbd_poll()`（`while(inb(SERIAL+5)&1)` 排空 FIFO → `buf_put` 到 256B 环形 `kbd_buf`）。
- `kbd_poll` 只在**主循环** `kbd_has_key()`（[kmain.c](file:///d:\BananaOS-Axion\src\kernel\kmain.c) step2）被调用；`kbd_try_get` 只取不轮询硬件。
- bash/readline raw 读：[devfs.c](file:///d:\BananaOS-Axion\src\kernel\fs\devfs.c) `tty_read` raw 分支 `while(tty_ibuf_len < need)`（VMIN 恒 1 → 每次返回 1 字节，一次 syscall）；取空后 `tty_getc` → `sched_block_and_switch(WAIT_KBD)` 阻塞。
- **机制推论**：bash 在用户态逐字节处理（每次 read syscall 之间、及处理回显期间），内核不排空串口；此窗口内 QEMU 灌入 >FIFO 容量的字节即溢出丢弃 → 丢字节。这解释了"一次突发必丢后半"且"与 host 分批粒度无关"（分批 25ms 仍落在 bash 处理窗口内）。
- 8259 PIC 已被禁用（`[INIT] Disabling 8259 PIC`），无时钟/串口中断驱动的后台排空可用；idt 通用门已装但协作式调度不依赖中断。

### 读盘链路
- [ata.c](file:///d:\BananaOS-Axion\src\kernel\dev\ata.c)：已加入 `ata_wait_done`（读后等命令完成+查 ERR）与 `ata_pio_read_verified`（同 LBA 双读/三读仲裁）——**本次构建已含、未做有效性验证**（后续测试被输入截断干扰）。
- [ext2.c](file:///d:\BananaOS-Axion\src\kernel\fs\ext2.c) `ext2_read_file` 逐块 `ext2_read_block(blk, ext2_blkbuf)` → memcpy 用户缓冲，逻辑正确；共享 `ext2_blkbuf` 无嵌套竞争（单核协作）。
- `wc -c/-l` 读同一文件正确 → 若双读校验生效，内容错位应消失（需重测确认）。

### 已完成（本会话）
- 阶段A：bash 登录循环（`g_bash_pid`/`sched_force_reap`/REAPED 槽复用）实现并自测通过（pid 复用 4，exit→自动重启×2）。
- ATA 加固两处（wait_done + 双读）已编译进当前 disk.img。
- fs.img 已注入 `/root/py_diag.py`（import 全清单诊断脚本）。
- 诊断/复现脚本群：`test_bash_restart.ps1`、`diag_py_imports.ps1`、`repro_one_import.ps1`、`iso_pure_runs.ps1`、`iso_s_imports.ps1`、`diag_wc.ps1`、`diag_grep_col.ps1`、`diag_py_see.ps1`、`diag_rundiag.ps1`（已写未跑）。

## 拟定改动

### Step 1：输入丢字节根因定位（实测探针，不改内核）
先确定丢字节发生在"内核接收"还是"bash/readline 消费"：
- 探针 A：boot 后 `cat`（无参数，回显 stdin→stdout），分批注入 300B 纯文本，数 cat 回显长度。cat 与 readline 不同路径（cat 用 stdio 缓冲大块 read）——若 cat 完整收到，则内核→文件描述符路径无损，问题在 bash/readline 逐字节消费窗口；若 cat 也丢，问题在 kbd_poll/主循环排空。
- 探针 B：bash 内建 `read -r line; printf '%s' "${#line}"` 注入已知长串测长度（内建 read 一次取一行，路径介于两者之间）。
- 探针 C：注入两段短命令间隔 1s（每段 <16B）确认小注入无丢 → 验证阈值。

产出：确定丢字节环节，写入结论。

### Step 2：按定位结果修复输入丢字节（内核改动，1-2 处）
按 Step1 结果从下列候选选取（执行时以实测为准，先做低风险项）：
- **候选 1（syscall 入口排空）**：在 syscall 公共路径（`syscall_entry.S` 切内核后、或 `syscall_dispatch` 入口）调用 `kbd_poll()` 一次——每次用户 syscall 顺带排空串口 FIFO。bash 逐字节 read 频繁进内核 → FIFO 水位被反复清空。改动小、无调度风险。
  - 文件：[syscall_entry.S](file:///d:\BananaOS-Axion\src\kernel\syscall\syscall_entry.S) / [syscall.c](file:///d:\BananaOS-Axion\src\kernel\syscall\syscall.c)；`keyboard.h` 已有 `kbd_poll`（需确认导出与否，改 static→extern 或加包装 `kbd_drain_serial()`）。
- **候选 2（tty raw 一次多返回）**：raw 分支 `need = min(VMIN, 可用)` 改为在 VMIN=1 且 kbd_buf 已有积压时一次性返回全部可用（`tty_ibuf` 可缓存到 count 上限），减少 bash 逐字节 syscall 往返。风险：改变 readline 语义（通常 readline 期望 1B/次，但多字节返回 readline 内部缓冲处理是标准做法，实测回归 test_shell_ux 确认）。
- **候选 3（host 侧批注 + 文档）**：若证明为 QEMU 模拟层瞬时溢出且真实串口/手工输入不触发，则将测试注入统一为分批慢发（已有雏形），文档注明限制，不额外改内核。
- 优先级：候选 1 → 候选 2 → 候选 3。改动后必须验证"单次注入 300B 长命令 echo 完整 + bash 执行成功"。

### Step 3：验证/收敛 ATA 双读对 python 内容错位的修复
- 跑 `python3 /root/py_diag.py`（短命令，规避输入问题；用分批 Send-Line 注入）。判定：
  - 全部模块 OK / 仅已知合理 FAIL（如 numpy 未装）→ ATA 双读有效，收尾。
  - 仍出现 `NameError/类缺失`（内容错位）→ 双读未覆盖。进一步：临时在 `ext2_read_file` 对每块做**再次 `ext2_read_block` 独立缓冲对比**或打印错位块号定位；确认是否同一文件稳定错位（确定性 vs 随机）。
  - 若 boot 时间因双读超标（约束 <2s，当前 boot_ms ~730ms）：将双读改为"错误怀疑才重读"或移除，另寻错位源。
- 同时确认 boot 时间未受 ATA 双读显著影响（日志 `Boot time`）。

### Step 4：回归
- `test_bash_restart.ps1`：bash 自动重启仍绿（输入路径改动不破坏）。
- 长命令输入：bash 执行 ≥200 字符单行命令成功（echo 完整、结果正确）。
- python 全清单：B3 合并 import（ssl/hashlib/sqlite3/ctypes/decimal/bz2/lzma/crypt/mmap/uuid/zoneinfo/termios/readline/asyncio）打印 `ALL-OK`。
- 若改动涉及 tty/readline 语义，补 `test_shell_ux.ps1`（9 项）回归。

### Step 5（后续阶段 C/D，计划内但按序后置）
- 阶段 C：perl→lua→ruby→node→go 逐个 `fetch_deps.py --packages` + 实测（当前 rootfs 已现 `/usr/share/perl5` 痕迹，可先探 perl）。
- 阶段 D：性能基线 + `PS1` 颜色/alias/EDITOR 注入（`fetch_deps.py` 的 `etc/bash.bashrc` / `.bashrc`），内核 Shell 补 `restart-bash` 命令。
- 阶段 D 的 `PS1` 注入需注意当前 prompt 无颜色但启动时输出 `BB BF` 两字节（疑 readline 在无 terminfo 下的定位序列）——与输入丢字节同查。

## 假设与决策
- 串口 FIFO 溢出发生在"bash 用户态窗口"而非 QEMU chardev 突发（host 分批无效佐证）——待 Step1 证实。
- ATA 双读是防内容错位的合理兜底；若拖慢 boot 则降级为仅怀疑时重读。
- python 库本身可用；本阶段不以"补新库"为目标，以"让现有库稳定可 import"为完成定义。
- 诊断脚本/日志保留在仓库根（与既有 test_*.ps1 惯例一致），不强制清理。

## 验证清单（完成定义）
1. 单次注入 ≥300B 命令行，echo 完整、bash 正常执行并回显结果（无截断/丢字）。
2. `python3 /root/py_diag.py` 中已装模块全部 OK 或仅 numpy 类缺失；`wc`/内容校验无错位。
3. `test_bash_restart.ps1` 通过（boot→ls→exit→自动重启×2）。
4. boot 时间日志 <2s 约束（对比改动前后 boot_ms）。
5. B3 合并 import 打印 `ALL-OK`。
