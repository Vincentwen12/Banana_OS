# Task 13: 修复键盘输入与输入后重启

## Summary

用户在 `./build.ps1 run`（QEMU -nographic）下启动系统后遇到两个问题：
1. bash 提示符 `bash-5.2#` 出现后**键盘输入没有回显、按回车报错**（"键盘输入不了"）
2. **输入命令后系统突然重启**（串口日志直接跳到 SeaBIOS）

探索发现根因集中在 **tty ioctl 子系统不完整**（sys_ioctl 只处理 TCGETS 且不填 termios 结构），以及**若干未验证的执行路径**（execve 失败路径等）。启动阶段日志还显示**早期随机重启**（hotness_init / doorbell_init 后各重启一次，属同批次问题）。

## Current State Analysis

### 键盘输入链路（已打通但体验错误）
- [keyboard.c](file:///d:/BananaOS-Axion/src/kernel/core/keyboard.c#L72-L94)：`kbd_poll()` 从串口（COM1，QEMU -nographic 的 stdin）读字符，`\r`→`\n`、`0x7F`→`\b`，存入 256 字节环形缓冲
- [devfs.c](file:///d:/BananaOS-Axion/src/kernel/fs/devfs.c#L40-L47)：`tty_read()` 调 `kbd_getchar()` 逐字符返回（无行缓冲、无回显）
- [table.c](file:///d:/BananaOS-Axion/src/kernel/syscall/table.c#L887-L902)：`sys_select_common()`（select/pselect6）无超时忙轮询 `kbd_has_key()`，bash readline 用它等 stdin
- **输入数据通路验证可用**：test_bash.ps1 通过管道注入命令已通过全部验收（echo/pwd/cd//bin/hello）

### 根因 1：sys_ioctl 残缺（"键盘输入不了 / 按回车报错" 的直接原因）
[table.c](file:///d:/BananaOS-Axion/src/kernel/syscall/table.c#L634-L642) 的 `sys_ioctl`：
```c
if (req == 0x5401) return 0;   /* TCGETS — 返回 0 但不填 termios 结构体 */
return (uint64_t)(-25);        /* 其余全部 ENOTTY */
```
后果（与用户日志逐条对应）：
- **TCGETS 不填结构体** → bash/readline 拿到全零 termios（无 ICANON/ECHO）→ readline 判断终端不回显、关闭行编辑 → **敲字无回显**
- **TCSETS (0x5402) 返回 ENOTTY** → readline 设置终端模式失败 → 日志 `readline: warning: turning off output flushing`，行编辑异常
- **TIOCSPGRP (0x5410) 返回 ENOTTY** → 日志 `bash: cannot set terminal process group (-1)` → bash 关闭 job control（`bash: no job control in this shell`）
- **TIOCGWINSZ (0x5413) 返回 ENOTTY** → readline 窗口尺寸未知，行为异常

### 根因 2：Windows 控制台行缓冲（输入"不及时"的体验问题）
QEMU -nographic 在 Windows 下 stdin 是行缓冲：用户敲字符被控制台缓冲，**按 Enter 才把整行提交到 COM1**。与根因 1 叠加 → 敲字无任何反应。这是 QEMU/终端环境限制，内核侧无法完全消除，但修复根因 1 后按 Enter 提交整行可正常回显执行。

### 疑点 3：输入后重启（需复现定位）
- **execve 失败路径未验证**：test_bash 只跑过存在的 `/bin/hello`。用户输入系统命令（ls/date 等不存在于 fs.img）→ bash fork → 子进程 execve → `elf_load` 失败返回 ENOENT（[table.c](file:///d:/BananaOS-Axion/src/kernel/syscall/table.c#L433-L437)）→ 子进程报错 exit(127)。此路径可能有不稳。
- 早期启动日志显示 **2 次随机重启**（hotness_init、doorbell_init 后），说明系统本身存在间歇性崩溃（QEMU TCG 时序 / 未初始化路径），可能与"输入后重启"是同一类问题。

### 已确认无问题
- AP 核心 `ap_entry` 只 `hlt`，不访问键盘（[kmain.c](file:///d:/BananaOS-Axion/src/kernel/kmain.c#L77-L91)），无多核键盘竞争
- `fd_table` 全局共享，单任务场景（bash 独占）可用
- `serial_putc` 阻塞等 THR 空，输出不丢字符

## Proposed Changes

### 1. 完整实现 sys_ioctl 的 tty 请求（核心修复）— [table.c](file:///d:/BananaOS-Axion/src/kernel/syscall/table.c#L634-L642)

把 `sys_ioctl` 从"只认 TCGETS 且不填数据"改为完整支持 bash/readline 需要的请求：

| 请求码 | 名称 | 行为 |
|---|---|---|
| 0x5401 | TCGETS | 向用户空间写一个 36 字节 termios 结构：`c_lflag = ICANON\|ECHO\|ISIG`，`c_oflag = OPOST\|ONLCR`，其余 0。让 bash 认为终端是标准 canonical+echo 交互终端 |
| 0x5402/0x5403/0x5404 | TCSETS/TCSETSW/TCSETSF | 接受设置，返回 0（不做实际状态变更） |
| 0x540F | TIOCGPGRP | 写 0 到用户空间（单进程组），返回 0 |
| 0x5410 | TIOCSPGRP | 返回 0（接受设置）→ 消除 "cannot set terminal process group" |
| 0x5413 | TIOCGWINSZ | 写 8 字节 winsize（80 列 × 24 行）→ readline 获得窗口尺寸 |
| 0x5414 | TIOCSWINSZ | 返回 0 |

需要新增：termios 结构体布局常量、`vm_copy_to_user` 写用户空间（已有该函数）。

### 2. tty 回显决策（devfs.c，实测后定）

修复根因 1 后，bash/readline 可能的行为有两种，需要实测决定：
- **若 readline 进入 canonical+echo 模式**：内核需在 `tty_read` 实现行缓冲 + 回显（收集到 `\n` 返回整行，读入字符立即 `vga_putc` 回显）。
- **若 readline 保持 raw 模式但自己回显**：无需内核回显。

先做变更 1，实测回显情况，再决定是否在 [devfs.c](file:///d:/BananaOS-Axion/src/kernel/fs/devfs.c#L40-L47) 的 `tty_read` 增加回显逻辑。**倾向方案**：在 tty_read 读字符后直接 `vga_putc(c)` 回显（对 canonical 模式必要；若 readline 也回显则出现双重回显，需实测确认）。

### 3. 输入后重启诊断与修复（复现驱动）

1. 写交互测试脚本 `test_interactive.ps1`：逐字符模拟真实终端输入（每字符间小延迟 + 回车），覆盖：空回车、内置命令、存在的外部命令、**不存在的命令（execve 失败路径）**。
2. 用 QEMU `-d int,cpu_reset -D qemu_int.log` 抓崩溃现场，定位具体异常（用户态 #GP/#PF）。
3. 若 execve 失败路径有 bug → 修复 `sys_execve` 失败分支（[table.c](file:///d:/BananaOS-Axion/src/kernel/syscall/table.c#L406-L460)）。
4. 若崩溃与早期随机重启同源 → 进入第 4 项。

### 4. 早期随机重启诊断

1. 对比测试：`-smp 4` vs `-smp 1`，确认是否多核 INIT-SIPI 时序导致（project_memory 有同类记录）。
2. 用 `-d int,cpu_reset` 抓早期崩溃现场。
3. 若属 QEMU TCG 多核时序：调整 AP 启动流程或减少默认核数（build.ps1 参数），并在文档注明。

## Assumptions & Decisions

- **修复目标**：bash 交互达到"命令可见、回车执行、错误报错不崩溃、连续操作不重启"。
- **canonical+echo vs raw**：先实现 TCGETS 返回标准交互 termios（canonical+echo），若 readline 行为异常再回退 raw 配置实测。以实测结果为准。
- **Windows 行缓冲**：无法在内核消除，接受"回车提交整行"的交互方式（修复后按 Enter 可看到命令整行出现并执行）。
- **早期随机重启**：若确诊为 QEMU TCG 多核时序（与代码无关），仅调整运行参数/文档说明，不改内核逻辑；若为代码 bug 则修复。
- 不动已通过验收的 fork/execve 正常路径与 sys_brk 修复。

## Verification

1. `./build.ps1` 构建通过（kernel.flat < 80KB）
2. `test_interactive.ps1` 逐字符注入验证：
   - 敲字符后命令整行回显（修复根因 1）
   - 空回车 → 新提示符，不报错不崩溃
   - `echo hi` / `pwd` / `cd /` → 正常输出
   - `/bin/hello` → 输出 `Hello from BananaOS ELF!` 且不崩溃
   - `ls`（不存在）→ bash 报 `command not found`，**不崩溃不重启**
   - 连续输入 5+ 条命令 → 无重启
3. 回归：`test_bash.ps1`（bash 验收）与 `test_regression.ps1`（hello/test_fork）全部 PASS
4. 多次冷启动（≥3 次）无早期随机重启；若有则按第 4 项处理
