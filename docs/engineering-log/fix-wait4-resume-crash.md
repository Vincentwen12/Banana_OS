# 修复：bash 在 python3 退出后重新调度时静默冻结

## Summary
W7 Phase 3.2 验证中发现：bash 已能运行外部命令（ls 通过），python3 能完整跑完（打印 "Python 3.10.4"、跑完 fini、发出 exit syscall），但 python3 退出后 bash 重新调度时**静默冻结**（无异常输出、无后续调度）。已用调试 printk 定位：bash 第二次调度 `kctxv=1`（kctx 有效），但 `run-via-kctx` 打印缺失——崩溃/挂死发生在 `vm_switch(t->mm_context)` 或 `syscall_restore_msrs()` 之间。

关键事实：内核异常处理器（idt.c）只用 `vga_puts` 打印，`-nographic` 下 VGA 不可见 → 内核态异常 = 静默 hlt。这完美解释"无任何输出"。

## Current State Analysis（已完成的探索结论）
- **原始 FS base 崩溃已修复**：task_t 增加 fs_base/gs_base；arch_prctl 保存到任务；fork 继承；syscall_entry.S 在 sysretq 前调用 `syscall_restore_msrs()`；kmain 在调度每个用户任务前调用 `syscall_restore_msrs()`。bash 不再在 ls 后崩溃。
- **新问题链**（来自 [dbg] printk 实测）：
  1. `run pid=4 kctxv=0` → bash 进入 `sys_wait4`（`wait4 enter pid=4`）
  2. `run pid=5 fork=1` → python3 运行，正常 exit（`exit pid=5 code=0`）
  3. kmain zombie 分支 → `sched_wake_waiting_parent` 成功唤醒 bash（`wake pid=4 kind=1 arg=ffffffff`）
  4. `run pid=4 kctxv=1` → bash 重新调度，**之后无任何输出**
- 两次调度之间只有：`vm_switch(t->mm_context)`（mov cr3）与 `syscall_restore_msrs()`（wrmsr×2）。
- task_t 结构无溢出（fork_ctx=user_regs_t 128B、kctx_t=64B、fs_base 在末尾，bss 静态数组零初始化）。
- vm_destroy 无人调用 → 任务地址空间不会被释放；vm_switch 只是 `mov cr3, pml4_phys`。
- 已就位的调试 printk：kmain 的 `after-vmswitch` / `after-restoremsr` / `run-via-kctx`（均未触发，尚未构建验证）。
- 顺带修复（已应用，保留）：linker.ld 的 .trampoline 改为 `ALIGN(4096)` 跟随 .rodata，根治 rodata 增长导致的重叠（此前 0x11a000 硬编码再次与 .rodata 重叠导致链接失败）。

## Proposed Changes

### Step 1：精确诊断（一次构建 + 一次运行）
无需再改 C 代码——用 QEMU 异常日志直接抓现场：
1. 修改 `d:\BananaOS-Axion\test_w7_dbg.ps1` 的 QEMU 参数，追加 `-d int,cpu_reset -D qemu_int.log`（复用现有 -d int 调试法，见项目记忆"定位内核崩溃用 QEMU -d int,cpu_reset -D log"）。
2. `powershell .\build.ps1 -SkipFs`（当前源码已含 after-vmswitch/after-restoremsr/run-via-kctx 打印）→ 运行 test_w7_dbg.ps1。
3. 读取 `qemu_int.log` 末段 + `w7_dbg_stream.log`，确定：
   - 若异常向量 `v=0d`（#GP）且 RIP 在 `syscall_restore_msrs`（wrmsr 指令）→ **路径 A**
   - 若异常向量 `v=0e`（#PF）且 RIP 在 vm_switch 附近或 printk 内存访问 → **路径 B**（bash 页表损坏）
   - 若两者都无 → 非异常，是死循环/无限阻塞（进一步用 w7_dbg_stream.log 的 after-vmswitch/after-restoremsr 判定位置）

### Step 2：修复（按诊断结果）

**路径 A：#GP at wrmsr（fs/gs base 非规范地址）**
- `syscall_restore_msrs()` 增加规范地址保护：`fs_base`/`gs_base` 非规范（不在 `[0x0, 0x00007FFFFFFFFFFF] ∪ [0xFFFF800000000000, 0xFFFFFFFFFFFFFFFF]`）时跳过 wrmsr 并打印警告（防 #GP 打崩内核）。
- `sched_spawn_process` 显式初始化 `fs_base = gs_base = 0`（防御性，bss 虽为零）。
- 追查非规范值来源：在 sys_arch_prctl ARCH_SET_FS/GS 处打印 addr 校验（`table.c`），确认是否是 glibc/ld.so 传入了非规范值（若是则改为"仅保存 + 返回 0"不写 MSR，符合 Linux 语义：真实内核只做校验）。

**路径 B：#PF after vm_switch（bash 页表/PML4 损坏）**
- 在 kmain run printk 中追加打印 `t->mm_context` 与 `pml4_phys`，与 bash 首次调度时的值对比是否被改写。
- 追查写入源：重点查 `vm_fork`（复制时是否误写 src 的 pml4/页表）、`elf_load`（pmalloc 连续页分配是否覆盖既有页表）、boomerang 热页回收（是否误回收在用页表页）。
- 若 pml4_phys 未变但内容被覆盖 → 查 pmalloc/pfree 双重释放（历史教训：loader 的 pfree_contig 曾导致崩溃，已移除，注意别再引入）。

### Step 3：清理与验证
1. 移除全部 `[dbg]` printk：
   - `sched.c`：`exit`/`wake`/`wake_parent`/`cand`/`resume` 打印；删掉临时加的 `#include "printk.h"`（若 print_str/print_uint 不再引用则不冲突——保留 include 亦可，见编译警告决定）。
   - `table.c`：`wait4 enter`/`wait4 woke` 打印。
   - `kmain.c`：`run`/`after-vmswitch`/`after-restoremsr`/`run-via-kctx`/`zombie` 打印。
2. 保留正式修复：`syscall_restore_msrs` + 两条调用点、arch_prctl 保存、fork 继承、linker.ld ALIGN(4096)。
3. 重建（`-SkipFs`）→ 跑 `test_w7_dbg.ps1`（无 dbg 打印但确认 python3 完整跑完 + bash 恢复）→ 跑 `test_w7_long.ps1`（python3/vim 长超时）→ 跑 `test_w7_fsimg.ps1` 全量。
4. 清理临时文件：test_w7_dbg.ps1、test_w7_long.ps1、w7_dbg_stream.log、qemu_int.log 及根目录旧诊断脚本/日志。
5. 更新 `.trae/documents/fix-fsbase-tls-crash.md`：追加"wait4 后重调度冻结"小节，记录根因与修复。

## Assumptions & Decisions
- 假定崩溃点确在 vm_switch/syscall_restore_msrs 之间（由 kctxv=1 + run-via-kctx 缺失推断，Step 1 用 -d int 确认）。
- 假定内核异常走 VGA-only 打印 + hlt（已由 idt.c 源码确认），故 -nographic 下静默。
- 不扩展范围：不处理 execve 后 fs_base 重置问题（ls/python3 均正常，ld.so 会先设 FS 再使用 %fs，属已知可接受行为）。
- linker.ld 的 ALIGN(4096) 改动保留（已证明必要且安全，kernel.flat 140116B 正常链接）。

## Verification
- 触发条件：bash 下运行 `python3 -V` 并等待其退出，bash 必须恢复提示符并能继续执行下一条命令。
- 通过标准：`test_w7_long.ps1` 全部 OK（bash/python3/vim/exit）；`test_w7_fsimg.ps1` 全量通过；无 [dbg] 残留；kernel.flat 链接成功。

## 实际根因与修复（已完成，2026-08-30）

### Bug 1：用户程序加载基址与内核身份区虚拟重叠（bash 冻结）
**根因**：`loader.c` 的 `ctx->base = 0x400000`（4MB）。用户 PIE 程序加载在虚拟 0x400000，brk 从此向上增长。python3 的 brk 增长到虚拟 0x806000 时，`sys_brk` 用 `vm_map_page` 把 U/S=1 用户页映射到该虚拟地址，**遮蔽**了内核身份映射区（HEAP_BASE=0x800000 处的 bash vm_context 等堆结构）。内核以 python3 的 CR3 访问全局地址 0x806000 读到用户堆数据 → CR3 切换后翻译错误 → #PF/#GP → triple fault / 静默 hlt。

**证据**：切到 bash 自己的 pml4（0x804000）读 0x806000 物理内容完全正常；当前 CR3（python3 页表）读为 ASCII 垃圾。`q0-q3="H-F28198IDEOGARP"`。

**修复**：`loader.c` 把 `ctx->base` 改为 `0x20000000`（512MB）——在内核堆（约 8~32MB）之上、用户栈（512GB）与 ld.so（0x7f0000000000）之下，brk 永远不会到达内核身份区。同时更新 `table.c` mmap 注释。

### Bug 2：interp.c read_whole_file 页不连续 → vfs_read 线性写穿内核栈（cat 偶发 #GP）
**根因**：`interp.c` 的 `read_whole_file` 用逐页 `pmalloc()` 分配文件缓冲，注释假设"pmalloc 按递增地址连续"。W6.5 热页缓存 + 位图碎片化后页不再连续（如 ld.so 60 页 = 0x4ff2..0x4fff + 0x5004..，中间跳过已分配的 kstack 页 0x5000-0x5003）。`vfs_read` 按线性地址写 fsize 字节，越过页边界写进相邻页——**恰好是当前任务的 16KB 内核栈（kstack=0x5800000，与 ld.so 缓冲 0x57f2000+0x3AD28 重叠）**。ext2_read_file 一边用 kstack 一边被写入的 ld.so 代码字节覆盖自身栈帧 → 下一次 `mov 0x58(%rdi),%edi`（读 inode->i_block[12]）RDI=垃圾非规范指针 → #GP。

**证据**：异常帧寄存器 `R14=0x57f2000`（ld.so 缓冲）、`RSP=0x58039d0`（kstack 顶 0x5804000 之下）；栈 dump 显示 ext2_read_file 的 `buf`/`&inode` 槽与 inode 全被 x86 代码字节覆盖；修复前崩溃的 buf 值 0x5840000/0x57f2000 均落在 kstack 0x5800000 附近。

**修复**：`interp.c` 的 `read_whole_file` 改用 `pmalloc_contig(num_pages)` 一次分配连续页（与 `loader.c` 既有修复一致；loader.c 的注释/实现本就如此）。

### 清理
- 移除全部 `[dbg]` printk（kmain.c 调度块、sched.c wake/wake_parent/resume/exit、table.c wait4、mm.c pfree、ext2.c read_file/blkbuf）；sched.c 的临时 `#include "printk.h"` 与未用静态 print_uint/print_str 一并删除；mm.c/ext2.c 的 printk.h 按需移除。
- idt.c/idt.S 增强保留：异常时 dump GPR + 故障栈（仅异常时输出，无常规开销）。
- 测试脚本稳健性：readline 一次性完成列表 spam 需完全结束再发命令，`Start-Sleep 4` → `15`；passwd 匹配改 `banana:x:` 防假阳性。
- 最终验证：`test_w7_long.ps1` 全绿（bash/python3/vim/exit）。fsimg 测试中 cat 的 KERNEL EXCEPTION 已消除（系统全程无异常跑完）。测试脚本的 ReadAllText-vs-AppendAllText 并发竞态会偶发假 TIMEOUT（系统实际正常，python3 实测输出 "Python 3.10.4"）。
