# W5 — Linux ABI 兼容层 (静态 ELF 运行) Spec

## Why
W1-W4 已构建完整的多核、多任务、弹性内存、功耗优化的自研内核，但只能运行内核内部任务（Shell、DemoA、DemoB），无法执行外部程序。W5 目标：加载并执行 Linux x86-64 静态 ELF 可执行文件，里程碑 `run /bin/bash` → 进入 bash-5.2$ 提示符。

## What Changes
- 新增 syscall/ 模块 (syscall 指令入口 + 系统调用表 + 约 15 个核心调用)
- 新增 elf/ 模块 (ELF64 解析器 + 静态链接加载器)
- 新增 fs/ 模块 (VFS 基础 + devfs + tmpfs)
- 修改 sched.c (用户态任务支持 + 进程管理)
- 修改 mm.c (mmap/munmap/brk 接口)
- 修改 shell.c (新增 run、ls、cat 命令)
- 修改 kmain.c (注册系统调用 + ELF 加载)
- 修改 axion.h (新增系统调用常量)
- 修改 build.ps1 (编译新文件)

## Impact
- Affected specs: W1 (shell), W2 (mm), W3 (sched/ipc)
- Affected code: syscall/, elf/, fs/, sched.c, mm.c, shell.c, kmain.c, axion.h, build.ps1

---

## ADDED Requirements

### Requirement: 系统调用框架 (syscall_entry.S + syscall.c)
系统 SHALL 通过 x86-64 `syscall` 指令提供用户态→内核态切换，包括 swapgs、栈切换、寄存器保存/恢复、sysretq 返回。

#### Scenario: 系统调用入口
- **WHEN** 用户态程序执行 `syscall` 指令
- **THEN** 内核保存所有寄存器，调用 `syscall_dispatch(nr, a1-a6)`，将返回值放入 rax，通过 `sysretq` 返回用户态

### Requirement: 系统调用表 (syscall/table.c)
系统 SHALL 提供与 Linux x86-64 兼容的系统调用号映射，W5 实现约 15 个核心调用。

| 编号 | 名称 | 功能 |
|------|------|------|
| 0 | sys_read | 从文件描述符读取 |
| 1 | sys_write | 写入文件描述符 |
| 2 | sys_open | 打开文件 |
| 3 | sys_close | 关闭文件描述符 |
| 9 | sys_mmap | 内存映射 |
| 11 | sys_munmap | 取消内存映射 |
| 12 | sys_brk | 调整堆大小 |
| 24 | sys_sched_yield | 主动让出 CPU |
| 39 | sys_getpid | 获取进程 ID |
| 59 | sys_execve | 执行程序 |
| 60 | sys_exit | 终止进程 |
| 63 | sys_uname | 获取系统信息 |
| 96 | sys_gettimeofday | 获取时间 |

#### Scenario: sys_exit(0)
- **WHEN** 用户态调用 `syscall(60, 0)`
- **THEN** 进程正常终止，返回 Shell

### Requirement: ELF 加载器 (elf/loader.c)
系统 SHALL 解析 ELF64 静态链接可执行文件，按 Program Headers 映射到内存，设置入口点。

#### Scenario: 加载 /bin/bash
- **WHEN** 调用 `elf_load("/bin/bash", &entry, &stack)`
- **THEN** 验证 ELF 魔数、类别、类型，遍历 PT_LOAD 段映射到内存，返回入口地址 0x402xxx 和栈顶

#### Scenario: 文件不存在
- **WHEN** 调用 `elf_load("/nonexistent", ...)`
- **THEN** 返回错误 "File not found"

#### Scenario: 非 ELF 文件
- **WHEN** 调用 `elf_load("/dev/null", ...)`
- **THEN** 返回错误 "Not a valid executable"

### Requirement: VFS 文件系统基础 (fs/vfs.c)
系统 SHALL 提供统一的文件操作接口（file_ops_t），支持 open/read/write/close/lseek/ioctl。

### Requirement: 设备文件系统 (fs/devfs.c)
系统 SHALL 提供 /dev/null、/dev/zero、/dev/tty 设备文件。

| 设备 | 读 | 写 |
|------|-----|-----|
| /dev/null | 返回 EOF | 丢弃 |
| /dev/zero | 返回 0 | 丢弃 |
| /dev/tty | 重定向 VGA/键盘 | 输出到 VGA |

### Requirement: 内存文件系统 (fs/tmpfs.c)
系统 SHALL 提供临时内存文件系统，支持创建文件和目录，用于存放测试 ELF 文件。

### Requirement: Shell 新增命令
系统 SHALL 在 Shell 中提供 run、ls、cat 命令。

#### Scenario: run /bin/bash
- **WHEN** 用户输入 `run /bin/bash`
- **THEN** 加载 ELF，创建用户态任务，执行 bash，显示 bash-5.2$ 提示符

#### Scenario: ls /bin
- **WHEN** 用户输入 `ls /bin`
- **THEN** 列出 /bin 目录下所有文件

#### Scenario: cat /etc/hostname
- **WHEN** 用户输入 `cat /etc/hostname`
- **THEN** 显示文件内容

### Requirement: 用户态任务
系统 SHALL 支持用户态任务的创建和执行，通过 syscall 与内核交互。

#### Scenario: 用户态任务执行
- **WHEN** `run /bin/hello` 执行
- **THEN** 输出 "Hello, world!"，任务正常退出

#### Scenario: 用户态栈溢出
- **WHEN** 用户态栈超过分配大小
- **THEN** Guard Page 检测到溢出，终止进程

## MODIFIED Requirements

### Requirement: 调度器扩展 (sched.c)
任务结构 SHALL 新增 pid、user_rsp、entry 字段，支持用户态任务的创建和调度。

### Requirement: 内存管理扩展 (mm.c)
内存管理器 SHALL 新增 mmap/munmap/brk 接口，支持 ELF 加载段的内存映射和堆管理。

### Requirement: BSP 主循环 (kmain.c)
BSP 初始化 SHALL 注册系统调用处理函数，初始化 ELF 加载器。

## Risk Mitigation
| 风险 | 影响 | 缓解措施 |
|------|------|------|
| 用户态栈溢出 | 内核崩溃 | 设置 Guard Page (4KB) 检测 |
| 系统调用参数错误 | 内核 Panic | 所有 syscall 入口增加参数校验 |
| ELF 文件格式错误 | 加载器崩溃 | 所有段解析加边界检查 |
| 用户态无限循环 | CPU 被占 | 调度器周期性检查，超时强制终止 |
| 用户态访问内核空间 | 安全漏洞 | 页表设置 U/S bit，用户态不可访问内核 |