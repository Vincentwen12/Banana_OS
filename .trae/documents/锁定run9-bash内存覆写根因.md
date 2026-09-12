# 锁定 run9 bash 内存覆写根因（重 import 负载下父进程返回槽被写为 1）

## 摘要
iso_s_imports.ps1 中 bash 连续 8 次成功 fork/exec 重 import 的 python 后，在第 9 次 fork python 的关键段（blocked {INT,TERM,CHLD}）崩溃：`user fault rip=1 cr2=1`——bash 执行了一个**被覆写为 1 的返回槽**（[ustack] 首字=1）。已排除：物理耗尽（used 半满、无 OOM）、内核信号源（无 itimer/kill 投递）、fs/gs 残留、fork 周期数（12× 轻量 python 全过）。触发条件 = **子进程重 import 负载**。目标：通过受控实验锁定"把 1 写进 bash 返回槽"的写入方并修复，验收 = iso_s_imports 10/10。

## 当前状态分析（基于已有取证 + 本次勘查）

### 已知事实
- 崩溃现场：`pid=4 rip=1 cr2=1 rsp=0x7ffffff4d0 pend=0 sigreq=0 h=0`；[ustack] `1 201a2630 f0 4000 100 80 7f00001d9989 70 6 201a9340 2019f660 201a9340 f0 70 201a2630 1201a9340`——混入 0x70/0xf0/0x100/0x4000 等**glibc chunk size 形态的小整数**与 0x201a2xxx/0x201a9xxx（bash brk 堆区指针）→ 形态像是 **bash 的控制流被恢复/跳转进了一段堆块数据**（siglongjmp/jmp_buf 或栈帧链中保存的 RSP 被改写，`ret` 弹出一个 chunk size/flags 字段 → rip=1）。
- 子进程（python，独立 mm，exec 后与 bash 无页面共享）的重负载为何能坏 bash：排除共享页后，只剩**全局共享的内核状态**（fd_table、ext2/vfs 内部缓冲、全局 syscall 上下文）与**内核写用户空间**两类通道。
- 判别实验：重 import 8 次后 run9 崩（3/3）；轻量 python 12 次不崩（1/1）→ 崩溃与"子进程做了多少次/多少内存操作"相关而与 fork 次数无关。

### 待验证通道（按嫌疑排序）
1. **vm_fork 深拷贝不彻底**：某页被父子共享（漏拷/漏映射），子进程（python exec 前的 fork 恢复期或 exec 失败路径）写入共享页 → 坏 bash。fork 后子 python 是 bash 的整 mm 拷贝，若共享任何一页且子进程在 exec 前写它 → bash 被污染。验证成本低、判定力强。
2. **bash 自身在 fork 关键段通过内核返回值/写用户空间的 syscall 把垃圾写入自身**（readline select/read 缓冲、wait4 status 等）——崩溃前的 syscall 是 bash 的 read + fork，均写自身正确地址。
3. **bash/glibc 堆自身 OOB**（非内核责任）——若是，需落到最小复现并评估 workaround。

## 建议改动（三个受控实验 + 按结果修复）

### 实验 E1 — fork 拷贝完整性核验（判定 vm_fork 是否共享/漏页）
文件：`src/kernel/syscall/table.c`（sys_fork）
- `vm_fork` 成功后、`sched_spawn_process` 前，插入一次性核验（诊断构建，验证后移除）：
  - 遍历 parent mm 全部用户 4KB 叶（复用 vm_fork 的四级走查逻辑）：
    - child 同 vaddr 必须 present；
    - child 物理页 ≠ parent 物理页（无共享）；
    - 两页内容逐字节相等。
  - 不符即 `printk("[forkchk] MISMATCH vaddr=%x p=%x c=%x\n", ...)`；全部通过打一次 `[forkchk] pages=%u ok`（按 pid 采样，如每 3 次 fork 打一行，避免刷屏）。
- 判定：任何 MISMATCH → **vm_fork 缺陷为根因**，直接修 vm.c（补漏拷/去共享），实验 3 免跑。
- 成本：bash 映射 ~15MB，逐 fork memcmp 一次，诊断期可接受。

### 实验 E2 — fault 现场寄存器完备化（定位 bash 崩溃函数与转移指令类型）
文件：`src/kernel/core/idt.S` + `src/kernel/core/idt.c`
- idt.S：`exc_common` 入口（`cli` 后、任何寄存器被复用前）把 15 个 GPR 存入全局 `fault_gpr[15]`（rax rbx rcx rdx rsi rdi rbp r8-r15，偏移与 user_regs_t 对齐即可，C 侧自定打印顺序）；`.bss/.data` 定义该数组。
- idt.c：`exc_kill_user` 的 [crash] 块追加一行 `[ugpr] rax=.. rbx=.. rbp=.. r12=.. r13=.. r14=.. r15=.. rsi=.. rdi=..`。
- 判定：由 RBP（栈帧基址）与 RIP=1 的转移（ret 则 rsp 弹 1；jmp/call *reg 则寄存器为 1）判断 bash 崩在哪个函数/结构：objdump bash 定位该函数，看它调用谁、保存的返回地址在哪——若 RBP 指向 heap 区 → 佐证"保存的 RSP/RBP 被改到堆"（jmp_buf/栈帧链损坏）；若 RBP 指向正常栈而返回槽=1 → 局部 OOB 写（栈上 struct/数组越界）。

### 实验 E3 — 最小复现曲线（定位"写入发生在哪一轮/哪个窗口"）
文件：新增 `probe_heavy_N.ps1`（复用 iso_s_imports 的串口注入模式）
- 参数化：(a) 每轮 import 模块数（1/3/7 档，从 os,json,urllib… 逐档加）；(b) 轮数；(c) 重/轻交替序列（如 5 重后接轻，观察崩溃发生在"第 N 次 fork"还是"重负载后的下一次 fork"）。
- 判定：确定崩溃所需的最小每轮负载与崩溃时机（fork 前 readline/execute，还是 fork 后 wait4 返回），把窗口从"8 轮"缩到"每轮 1-2 个模块 × 3-5 轮"，配合 E1/E2 结果收窄写入方。

### 修复分支（按证据决策）
| E1 结果 | 定因 | 修复 |
|---|---|---|
| forkchk MISMATCH | vm_fork 漏拷/共享页 | 修 `src/kernel/mm/vm.c` vm_fork：补拷缺失页 / 纠正映射逻辑；回归验证 |
| forkchk 全通过，E2 显示 RBP 指向堆区 | bash 保存帧链/控制结构被改到堆（无内核共享页则只能是 bash/glibc 自身堆 OOB 或某 syscall 返回值驱动 bash 写错） | 深查 E2 定位的 bash 函数及其依赖的 syscall 语义（对照 Linux），修内核返回语义或对 bash 做 workaround（见下） |
| E2 显示 RBP 正常、返回槽被单点覆写 | 栈上小结构越界（bash 或内核某 write 到错误偏移） | 按覆写偏移反推写入者：若为内核 vm_copy_to_user 调用，审计该调用的目标地址来源 |

若最终判定为 bash/glibc 在无共享页下的自身 OOB（工具链缺陷而非内核），则给出可落地 workaround 候选并按需实施其一：
- bash fork 前 `MALLOC_PERTURB_`/`MALLOC_CHECK_` 环境变量注入（rootfs 侧）；
- 升级/替换 rootfs 中 bash 二进制（若宿主工具链版本差异所致）；
- 把 iso 用例改为每轮独立 bash（`bash -c`）规避长会话累积（仅测试侧）。

## 假设与决策
- 崩溃循环已被 `syscall_sig_active` 护栏打断（run9 单次崩后登录循环可恢复）；本计划专注**首次** rip=1 的写入方，不回归已有信号修复。
- E1/E2 为诊断构建，确认根因后移除插桩；E3 探针脚本验证后删除。
- 全程保留已定案改动：mmap 碰撞避让 + 真实 munmap + 信号护栏/卫生 + itimer 卫生。

## 验证步骤
1. 实施 E1+E2（同一次构建），重建 `build.ps1 -SkipFs`，跑 `iso_s_imports.ps1` 一次：
   - 期望拿到：run1-8 OK 期间的 [forkchk] 结果（若 MISMATCH → 直接定位）+ run9 [crash] 的 [ugpr] 行与 [ustack]。
2. 依 E1/E2 结果实施对应修复分支。
3. 重建并跑 `iso_s_imports.ps1`：目标 **10/10 OK**。
4. 回归：`test_bash_restart.ps1`（bash 登录重启）；重跑 E3 探针确认最小复现消失。
5. 移除诊断插桩（[forkchk]/[ugpr]/[ustack]/[mem]/[oom]/[sigdbg]），重建复验一次。

## 影响范围
- 诊断期修改：`src/kernel/syscall/table.c`（forkchk）、`src/kernel/core/idt.S`（fault_gpr）、`src/kernel/core/idt.c`（[ugpr] 打印）、新增 `probe_heavy_N.ps1`。
- 修复期可能：`src/kernel/mm/vm.c`（vm_fork）、`src/kernel/syscall/table.c`（syscall 语义）、`tools/rootfs`（workaround）。
- 不动：brk/mmap/munmap 定案改动、信号护栏、调度主循环。
