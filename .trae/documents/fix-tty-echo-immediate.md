# 修复：bash 输入字母不即时回显，按编辑键才显示

## Summary

在 `bash-5.2#` 提示符下打字时，字母不回显（或延迟成批出现），按方向键 / Tab / 退格等非字母键时，之前打的字才一次性显示。根因是 **readline 处于降级 dumb 终端模式**（无 TERM 环境变量、无 terminfo 数据库，启动日志可见 `readline: warning: turning off output flushing`）：它把字符回显缓冲、延迟刷新，仅在编辑类按键触发重绘时才把行内容写回终端；与此同时 readline 的 raw 模式 TCSETS 清掉了我们 tty 记录的 `ECHO` 标志，内核也不再回显 → 双方都不回显，字母不可见。

修复：让内核 tty **无条件即时回显输入字符**（不再受 readline 清掉的 ECHO 位限制）。在无 terminfo 的当前环境下 readline 从不即时回显，因此不会双重回显；readline 后续的延迟重绘用 `\r`+整行重写覆盖原位，与内核回显内容一致，显示干净。

## Current State Analysis

### 输入链路（已确认实时到达）
- 用户运行环境为 **Windows Terminal**（PTY），字符实时到达 QEMU COM1，非控制台行缓冲。
- [keyboard.c](file:///d:/BananaOS-Axion/src/kernel/core/keyboard.c#L72-L94)：`kbd_poll()` 排空串口，`\r`→`\n`、`0x7F`→`\b`，存入 256 字节环形缓冲。
- [table.c](file:///d:/BananaOS-Axion/src/kernel/syscall/table.c#L949-L964)：`sys_select_common()` 无超时轮询 `kbd_has_key()`，readline 用它等 stdin，字符可被逐字符读出。

### 回显缺失的机制（根因）
1. readline 初始化时 `tcgetattr`/`tcsetattr`：TCGETS 拿到我们返回的 `c_lflag=0x000B`（ISIG|ICANON|ECHO），随后 readline 的 raw 模式 TCSETS 清掉 ICANON/ECHO → [table.c](file:///d:/BananaOS-Axion/src/kernel/syscall/table.c#L685-L702) 的 TCSETS 分支记录 `c_lflag=0`。
2. [devfs.c](file:///d:/BananaOS-Axion/src/kernel/fs/devfs.c#L66-L74) 的 `tty_read` 仅在 `tty_c_lflag & TTY_ECHO` 时回显 → readline 清掉 ECHO 后内核不再回显。
3. readline 因无 TERM/terminfo 处于 dumb 模式（日志 `turning off output flushing`），把自回显缓冲、延迟到重绘事件（方向键 / Tab / 退格 / 回车）才输出 → 用户看到"打字不显示、按编辑键才显示"。
4. 验证过的直接证据：此前 test_interactive（字符带 120ms 延迟注入）里 `echo hi` 能显示，是因为按回车触发 readline 整行重绘，与用户"按非字母键才显示"是同一机制。

### 为什么无条件内核回显不会双重回显
- readline（dumb 模式）从不即时回显单个字符，只做延迟整行重绘。
- readline 延迟重绘用 `\r`（carriage return）+ prompt + 整行重写，覆盖终端当前位置（[vga.c](file:///d:/BananaOS-Axion/src/kernel/core/vga.c#L58-L63) 已原生处理 `\r` 归列），与内核已回显的相同内容原位覆盖 → 显示一次、干净。
- 内核 shell（`>` 提示符）走 [kmain.c](file:///d:/BananaOS-Axion/src/kernel/kmain.c#L299-L308) 直接 `kbd_getchar()+vga_putc`，不经 tty_read，不受影响。

## Proposed Changes

### 1. devfs.c — tty_read 无条件即时回显（核心修复）

文件：[src/kernel/fs/devfs.c](file:///d:/BananaOS-Axion/src/kernel/fs/devfs.c#L66-L74)

把 `tty_read` 中的回显分支从"受 ECHO 位控制"改为"无条件回显"：

```c
static uint64_t tty_read(void* f, uint64_t off, void* buf, uint64_t size)
{
    (void)f; (void)off;
    if (size == 0) return 0;
    char c = kbd_getchar();
    /* 即时回显：本环境 readline 处于 dumb 模式（无 terminfo），从不即时回显；
     * 它的 raw TCSETS 会清掉 ECHO，若内核也按 ECHO 位回显则双方都不回显。
     * 内核作为终端必须回显。readline 的延迟整行重绘用 \r 原位覆盖，不会重复。 */
    if (c == '\n')      vga_putc('\n');
    else if (c == '\b') vga_puts("\b \b");
    else                vga_putc(c);
    ((char*)buf)[0] = c;
    return 1;
}
```

要点：
- 删除 `if (tty_c_lflag & TTY_ECHO)` 门控，保留 `\n`/`\b` 的专门回显处理。
- `tty_c_lflag`/`tty_c_oflag` 记录与 `devfs_tty_get_flags`/`devfs_tty_set_flags` 保持不变（TCGETS 仍向 readline 报告标准交互 termios，不影响本修复）。
- `tty_write` 不变。

### 2. test_interactive.ps1 — 增加"快速打字即时回显"校验

文件：[test_interactive.ps1](file:///d:/BananaOS-Axion/test_interactive.ps1)

在现有 Test 1 之前新增一个快速打字校验块（模拟真实打字速度，验证字母在按回车**之前**已出现在输出流中）：

- 用 `Send-Typed` 但把逐字符延迟从 120ms 降到 0（或新写一个 `Send-FastTyped`，逐字符无延迟）。
- 发送 `echo fast-echo` 后**先不按回车**，等待 1-2 秒，检查 captured stream 中已出现 `echo fast-echo`（证明字母即时回显，而非回车后才显示）。
- 再按回车，检查输出 `fast-echo` 且无重复行、无重启。
- 现有 8 个测试保持不变。

### 3. （不实现，仅记录回退方案）若验证出现双重回显

若快速打字测试发现字母重复（readline 延迟重绘 + 内核回显叠加），回退方案：向 fs.img 注入一个最小 terminfo（仅含 `cr=\r`、`nw=\r\n`、`bs=^H`、`cols=80`、`lines=24`，不含任何 ANSI 转义），并在 `sys_execve` 的 `elf_build_user_stack` 环境里传 `TERM` 环境变量，让 readline 脱离 dumb 模式正常即时回显。本计划不实现该回退，仅记录。

## Assumptions & Decisions

- **根因**：readline dumb 终端模式的延迟回显 + 内核 ECHO 位被 readline raw TCSETS 清除，双方都不回显。不涉及 Windows 终端行缓冲（用户环境为 Windows Terminal，输入实时到达，已通过 AskUserQuestion 确认）。
- **修复取向**：内核无条件回显。在无 terminfo 环境下 readline 从不即时回显，故不会双重回显；readline 延迟重绘为 `\r` 原位覆盖，显示干净。
- **不做**：不引入 terminfo 数据库与 TERM 环境变量（改动大、需手工构造 terminfo 二进制、且 VGA 不解析 ANSI 转义，超出本次"回显"问题范围）。
- **保留**：现有 termios 记录 / TCGETS 报告 / TCSETS 接收逻辑、`\r→\n`、`0x7F→\b` 归一化、select/pselect6 轮询。
- 内核 shell 与 bash 的 read(0) 均受益；`run` 启动的非交互程序不受影响。

## Verification

1. `.\build.ps1` 构建通过，`kernel.flat < 80KB`（当前 78492 字节）。
2. 快速打字测试（test_interactive.ps1 新增块）：
   - 发送 `echo fast-echo`（无延迟）后**未按回车**，输出流已出现 `echo fast-echo`（字母即时回显）。
   - 按回车后输出 `fast-echo`，无重复字符/重复行。
   - 全程无重启（`Booting from Hard Disk` 仅 1 次）。
3. 回归：
   - `test_interactive.ps1`（含原 8 个测试）全部 PASS。
   - `test_bash.ps1` 全部 PASS。
   - `test_regression.ps1`（hello + test_fork）全部 PASS。
4. 手动体验：Windows Terminal 中运行 `.\build.ps1 run`，在 `bash-5.2#` 打字，字母随敲随显；按退格可删；回车正常执行。
