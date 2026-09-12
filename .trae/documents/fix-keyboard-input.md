# 修复键盘输入无响应

## Summary
系统启动后 Shell 提示符 `>` 正常显示，但在终端敲击键盘无任何反应。根因是输入路径错配：键盘驱动只轮询 PS/2 端口（0x60/0x64），而 `build.ps1` 的 `run`/`debug` 目标使用 QEMU `-nographic` 模式，该模式下终端 stdin 经由 COM1 串口（0x3F8）注入，PS/2 键盘无输入源（无图形窗口）。驱动读不到任何键，导致输入完全失效。

## Current State Analysis

[build.ps1](file:///d:/BananaOS-Axion/build.ps1) 的 QEMU 启动参数：

- `run`（第 188 行）：`-nographic`，stdin/stdout 经串口 COM1
- `debug`（第 200 行）：`-nographic`
- `run-gui`（第 194 行）：图形模式，PS/2 键盘可用

[keyboard.c](file:///d:/BananaOS-Axion/src/kernel/core/keyboard.c) 的输入读取唯一入口是 `kbd_poll()`（第 72-97 行），仅轮询 PS/2：

```c
static void kbd_poll(void) {
    if (!(inb(PS2_STATUS_PORT) & 0x01)) return;  // 只读 PS/2
    uint8_t sc = inb(PS2_DATA_PORT);
    ...
}
```

[vga.c](file:///d:/BananaOS-Axion/src/kernel/core/vga.c) 的 `serial_putc`（第 9-12 行）已经通过 `SERIAL_PORT`（0x3F8）输出字符，但内核没有任何地方读串口 *输入*（`inb(SERIAL_PORT)`）。`SERIAL_PORT` 定义在 [axion.h](file:///d:/BananaOS-Axion/src/include/axion.h#L79) 为 `0x3F8`，其 LSR（Line Status Register）位于 `SERIAL_PORT + 5`，bit 0 为 Data Ready。

输入读取的消费方：
- [kmain.c](file:///d:/BananaOS-Axion/src/kernel/kmain.c#L274) 通过 `kbd_getchar()` 读取按键并回显
- [devfs.c](file:///d:/BananaOS-Axion/src/kernel/fs/devfs.c#L44) 的 tty 读取也调用 `kbd_getchar()`

## Proposed Changes

### 文件: src/kernel/core/keyboard.c

在 `kbd_poll()` 函数体开头增加串口输入读取，与 PS/2 并存，覆盖 `-nographic`（串口）与 `run-gui`/真机（PS/2）两种输入源：

```c
static void kbd_poll(void) {
    /* 串口输入: QEMU -nographic 下终端 stdin 经由 COM1 注入 */
    while (inb(SERIAL_PORT + 5) & 0x01) {
        char c = (char)inb(SERIAL_PORT);
        if (c == '\r')      c = '\n';   /* Enter 归一化为换行 */
        else if (c == 0x7F) c = '\b';   /* Backspace 归一化为退格 */
        buf_put(c);
    }

    /* PS/2 键盘: run-gui / 真机下使用 */
    if (!(inb(PS2_STATUS_PORT) & 0x01)) return;
    ...原有 PS/2 逻辑保持不变...
}
```

要点：
- LSR（`SERIAL_PORT + 5`）bit 0 表示接收 FIFO 有数据；用 `while` 一次性排空，降低按键延迟。
- 归一化 `\r`→`\n`、`0x7F`→`\b`，让 shell 输入循环的换行/退格分支正确命中（kmain 与 `kbd_readline` 均已处理 `\n`/`\r` 与 `\b`/`0x7F`，归一化后语义更一致）。
- 保留 PS/2 轮询分支，不破坏 `run-gui` 与真实硬件的键盘支持。

> 不删除 keyboard.c 第 85-93 行的 PS/2 串口调试输出（`'K'...`）：它们在 `-nographic` 下因无 PS/2 输入不会触发，且超出本次"输入修复"范围。

## Assumptions & Decisions
- 根因是 `-nographic` 下 stdin 走串口而非 PS/2，非 PS/2 驱动 bug；因此采用"串口 + PS/2 双读"而非修改 QEMU 参数（保持 `run` 的 nographic 无头运行不变）。
- 复用已定义的 `SERIAL_PORT` 常量，不新增宏。
- 输入回显由 kmain 已有的 `vga_putc()` 承担（`vga_putc` → `serial_putc` 输出到终端），无需额外回显逻辑。

## Verification
1. 运行 `./build.ps1 run`，编译通过无新增警告。
2. 在 Shell 提示符 `>` 后键入 `help` 并回车，确认字符正常回显且命令被执行（输出帮助文本）。
3. 键入 `run /bin/bash` 回车，确认输出 `Hello from BananaOS ELF!`。
4. 测试退格键可删除字符，Enter 正确提交命令。
5. 确认 `Booting from Hard Disk` 仅出现 1 次，无重启、无杂乱调试输出。