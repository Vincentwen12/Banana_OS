# Fix `run /bin/bash` - ELF Loading & Execution

## Summary

`run /bin/bash` 报 "File not found" 后，虽然已注册 `/bin/bash` 到 tmpfs，但 ELF 加载成功后用户任务无法实际执行。需要修复 3 个层面：文件注册、用户任务调度分发、测试 ELF 的 Ring 0 兼容性。

## Current State Analysis

### 问题 1: `/bin/bash` 未注册 ✅ 已修复
- [hello_init.c](file:///d:/BananaOS-Axion/src/kernel/elf/hello_init.c) 已添加 `tmpfs_create_file("/bin/bash", ...)`，与 `/bin/hello` 指向同一测试 ELF。

### 问题 2: 用户任务调度分发未实现
- `sched_create_user_task()` 创建的任务 `func=NULL`、`is_user=1`
- [kmain.c](file:///d:/BananaOS-Axion/src/kernel/kmain.c) 主循环（L288 和 L305）: `if (t->func) t->func()` — 用户任务 `func=NULL`，永远不会执行
- 没有代码路径跳转到 `t->entry` 执行用户代码

### 问题 3: 测试 ELF 使用 `syscall` 指令（Ring 0 下非法）
- [hello_elf.h](file:///d:/BananaOS-Axion/src/kernel/elf/hello_elf.h) 中的 ELF 二进制使用 `syscall` (0x0F 0x05) 指令
- `syscall` 在 Ring 0 下会触发 #UD（未定义操作码）异常
- W5 阶段无 IDT 异常处理，会导致 Triple Fault → 内核重启
- [syscall_entry.S](file:///d:/BananaOS-Axion/src/kernel/syscall/syscall_entry.S) 注释也确认：W5 用户程序运行在 Ring 0，应直接调用 `syscall_dispatch`

### 问题 4: 用户任务无 stdin/stdout/stderr
- `sys_write(fd=1, ...)` 需要 fd 1 指向可写设备
- 用户任务创建时未分配文件描述符
- [devfs.c](file:///d:/BananaOS-Axion/src/kernel/fs/devfs.c) 已实现 `/dev/tty` 设备（tty_write → VGA），可直接使用

## Proposed Changes

### 1. 修改 `linker.ld` — 为 trampoline 预留固定地址

**文件**: [linker.ld](file:///d:/BananaOS-Axion/linker.ld)

在 `.text` 段之前添加 `.trampoline` 段，固定在 0x100000 处（内核入口，与 `_start` 相同地址，但 trampoline 是独立函数不影响启动）：

```ld
.trampoline 0x100000 : {
    KEEP(*(.trampoline.text))
}
```

实际上 trampoline 放在 `.text` 中即可，通过 `objdump -t kernel.bin | grep syscall_trampoline` 获取地址后硬编码到 ELF 生成脚本中。但更稳健的做法是放在固定地址。

**最终方案**: 在 `.text` 段末尾之后、`.rodata` 之前插入固定地址的 trampoline 段：

```ld
/* Syscall trampoline — 固定地址，ELF 测试程序通过 call 到此地址 */
. = 0x101000;
.trampoline : {
    *(.trampoline)
}
```

### 2. 添加 `syscall_trampoline` 函数

**文件**: [syscall_entry.S](file:///d:/BananaOS-Axion/src/kernel/syscall/syscall_entry.S)

在文件末尾添加 trampoline 函数，放在 `.trampoline` 段中：

```asm
/* Syscall trampoline for Ring 0 user tasks (W5)
 * Replaces the syscall instruction which is illegal in Ring 0.
 * Calling convention: rax=syscall_nr, rdi=arg1, rsi=arg2, rdx=arg3,
 *                     r10=arg4, r8=arg5, r9=arg6
 * Returns: rax=result
 */
.section .trampoline, "ax"
.global syscall_trampoline
.type syscall_trampoline, @function
syscall_trampoline:
    pushq %rcx
    pushq %r11
    pushq %r10
    pushq %r8
    pushq %r9
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %rax

    /* syscall_dispatch(nr, a1, a2, a3, a4, a5, a6) */
    movq %rax, %rdi     /* arg1 = syscall number */
    /* rsi already = arg2 */
    /* rdx already = arg3 */
    movq %r10, %rcx     /* arg4 (x86-64 ABI: 4th arg in rcx) */
    movq %r8,  %r8      /* arg5 */
    movq %r9,  %r9      /* arg6 */
    call syscall_dispatch

    /* Restore registers (rax has return value, keep it) */
    popq %r11            /* discard saved rax */
    popq %rdx
    popq %rsi
    popq %rdi
    popq %r9
    popq %r8
    popq %r10
    popq %r11
    popq %rcx
    ret
.size syscall_trampoline, . - syscall_trampoline
```

### 3. 修改 `gen_hello_elf.py` — 替换 `syscall` 为 `call trampoline`

**文件**: [tools/gen_hello_elf.py](file:///d:/BananaOS-Axion/tools/gen_hello_elf.py)

将 `syscall` (0x0F 0x05, 2 bytes) 替换为 `call syscall_trampoline` (0xE8 + rel32, 5 bytes)。

由于代码段变长 6 bytes (2次替换，每次多 3 bytes)，需要重新计算偏移量。

trampoline 地址: 0x101000（通过 linker.ld 固定）
ELF 加载地址: 0x400000

第一次 `call` 位置: entry + 30 = 0x400078 + 30 = 0x400096
  - 下一条指令地址: 0x400096 + 5 = 0x40009B
  - rel32 = 0x101000 - 0x40009B = 0xFFD00F65

第二次 `call` 位置: 第一次 call 之后 + 12 = 0x40009B + 5 + 7 + 3 + 5 = 0x4000AF
  - 下一条指令地址: 0x4000AF + 5 = 0x4000B4
  - rel32 = 0x101000 - 0x4000B4 = 0xFFD00F4C

修改后的代码：
```python
code = bytes([
    # write(1, msg, 25)
    0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00,  # mov $1, %rax
    0x48, 0xC7, 0xC7, 0x01, 0x00, 0x00, 0x00,  # mov $1, %rdi
    0x48, 0x8D, 0x35, 0x18, 0x00, 0x00, 0x00,  # lea 24(%rip), %rsi (adjusted for new code size)
    0x48, 0xC7, 0xC2, 0x19, 0x00, 0x00, 0x00,  # mov $25, %rdx
    0xE8, 0x65, 0x0F, 0xD0, 0xFF,              # call syscall_trampoline
    # exit(0)
    0x48, 0xC7, 0xC0, 0x3C, 0x00, 0x00, 0x00,  # mov $60, %rax
    0x48, 0x31, 0xFF,                             # xor %rdi, %rdi
    0xE8, 0x4C, 0x0F, 0xD0, 0xFF,              # call syscall_trampoline
])
```

需要重新计算 `code_offset`、`entry`、`filesz` 等。

### 4. 修改 `kmain.c` — 添加用户任务分发路径

**文件**: [kmain.c](file:///d:/BananaOS-Axion/src/kernel/kmain.c)

在两处调度循环（L282-291 和 L300-308）中，在 `if (t->func)` 之前添加用户任务处理：

```c
if (t->is_user && t->entry) {
    /* W5: Ring 0 user task dispatch */
    uint64_t saved_rsp;
    __asm__ volatile("movq %%rsp, %0" : "=r"(saved_rsp));
    __asm__ volatile("movq %0, %%rsp" :: "r"(t->user_rsp));

    void (*entry)(void) = (void (*)(void))t->entry;
    entry();

    __asm__ volatile("movq %0, %%rsp" :: "r"(saved_rsp));
    sched_user_task_exit(t->id);
} else if (t->func) {
    t->func();
}
```

### 5. 修改 `sched_create_user_task` — 分配 stdin/stdout/stderr

**文件**: [sched.c](file:///d:/BananaOS-Axion/src/kernel/sched/sched.c)

在 `sched_create_user_task` 末尾添加（需要引入 `fs/vfs.h` 和 `fs/devfs.h`）：

```c
/* Allocate stdin/stdout/stderr */
file_t* f = devfs_open("/dev/tty", 0);
if (f) vfs_fd_alloc(f);  /* fd 0 = stdin */
f = devfs_open("/dev/tty", 0);
if (f) vfs_fd_alloc(f);  /* fd 1 = stdout */
f = devfs_open("/dev/tty", 0);
if (f) vfs_fd_alloc(f);  /* fd 2 = stderr */
```

### 6. 重新生成 `hello_elf.h`

运行 `python tools/gen_hello_elf.py` 重新生成 [hello_elf.h](file:///d:/BananaOS-Axion/src/kernel/elf/hello_elf.h)。

## Assumptions & Decisions

1. W5 阶段用户程序运行在 Ring 0（内核态），通过 `call syscall_trampoline` 触发系统调用
2. `syscall_trampoline` 固定放置在 0x101000（通过 linker.ld 的 `.trampoline` 段）
3. [devfs.c](file:///d:/BananaOS-Axion/src/kernel/fs/devfs.c) 已实现 `/dev/tty` 设备（tty_write → VGA），无需修改
4. 用户任务退出后通过 `sched_user_task_exit` 标记为 zombie
5. 用户任务使用独立的 4MB 栈（由 `elf_load_from_mem` 分配）

## Files to Modify

| 文件 | 修改内容 | 状态 |
|------|---------|------|
| [hello_init.c](file:///d:/BananaOS-Axion/src/kernel/elf/hello_init.c) | 添加 `/bin/bash` 注册 | ✅ 已完成 |
| [linker.ld](file:///d:/BananaOS-Axion/linker.ld) | 添加 `.trampoline` 段固定地址 0x101000 | 待实施 |
| [syscall_entry.S](file:///d:/BananaOS-Axion/src/kernel/syscall/syscall_entry.S) | 添加 `syscall_trampoline` 函数 | 待实施 |
| [gen_hello_elf.py](file:///d:/BananaOS-Axion/tools/gen_hello_elf.py) | 替换 `syscall` → `call syscall_trampoline` | 待实施 |
| [hello_elf.h](file:///d:/BananaOS-Axion/src/kernel/elf/hello_elf.h) | 重新生成 | 待实施 |
| [kmain.c](file:///d:/BananaOS-Axion/src/kernel/kmain.c) | 添加用户任务分发路径 | 待实施 |
| [sched.c](file:///d:/BananaOS-Axion/src/kernel/sched/sched.c) | 分配 stdin/stdout/stderr | 待实施 |

## Verification

1. `.\build.ps1` 构建成功，kernel.bin < 80KB
2. QEMU 启动后，`run /bin/bash` 输出：
   ```
   ELF: fsize=<size>
   ELF: bytes_read=<size>
   ELF loaded: entry=0x400078, stack=0x...
   Started task <N> (entry=0x400078)
   Hello from BananaOS ELF!
   ```
3. 用户任务执行完毕后不会导致系统崩溃（不再重启）
4. `ps` 显示用户任务状态为 zombie
5. 系统可以继续接收命令