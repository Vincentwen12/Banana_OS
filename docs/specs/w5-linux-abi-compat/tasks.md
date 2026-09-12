# Tasks

- [x] Task 1: 创建 syscall 模块 (系统调用框架)
  - [x] 创建 `src/kernel/syscall/` 目录
  - [x] 实现 `syscall_entry.S` (swapgs + 寄存器保存/恢复 + syscall_dispatch + sysretq)
  - [x] 实现 `syscall.c/h` (syscall_dispatch 分发函数)
  - [x] 实现 `syscall_init()` 注册系统调用处理函数

- [x] Task 2: 实现系统调用表 (syscall/table.c)
  - [x] 创建 `syscall/table.c/h`
  - [x] 实现 sys_read (从 fd 读取)
  - [x] 实现 sys_write (写入 fd)
  - [x] 实现 sys_open (打开文件)
  - [x] 实现 sys_close (关闭 fd)
  - [x] 实现 sys_mmap (内存映射)
  - [x] 实现 sys_munmap (取消映射)
  - [x] 实现 sys_brk (调整堆大小)
  - [x] 实现 sys_sched_yield (让出 CPU)
  - [x] 实现 sys_getpid (获取进程 ID)
  - [x] 实现 sys_execve (执行程序)
  - [x] 实现 sys_exit (终止进程)
  - [x] 实现 sys_uname (系统信息)
  - [x] 实现 sys_gettimeofday (获取时间)

- [x] Task 3: 实现 ELF 加载器 (elf/loader.c)
  - [x] 创建 `src/kernel/elf/` 目录
  - [x] 实现 ELF 头验证 (魔数、ELFCLASS64、ET_EXEC)
  - [x] 实现 Program Headers 遍历 (PT_LOAD 映射到内存)
  - [x] 实现 PT_INTERP 检测 (W5 仅打印警告，W6 加载动态链接器)
  - [x] 实现用户栈分配 (4MB，Guard Page 保护)
  - [x] 实现 `elf_load()` 和 `elf_load_from_mem()` 接口
  - [x] 实现 `elf_unload()` 释放资源

- [x] Task 4: 实现 VFS 文件系统基础 (fs/)
  - [x] 创建 `src/kernel/fs/` 目录
  - [x] 实现 `vfs.c/h` (file_ops_t 接口 + file_t 结构)
  - [x] 实现 `devfs.c/h` (/dev/null, /dev/zero, /dev/tty)
  - [x] 实现 `tmpfs.c/h` (内存文件系统，支持创建文件/目录)

- [x] Task 5: 扩展内存管理 (mm.c 修改)
  - [x] 新增 mmap/munmap 接口 (用户空间映射)
  - [x] 新增 brk 接口 (堆管理)
  - [~] 页表设置 U/S bit (W6 实现 — W5 使用 Ring 0 身份映射)

- [x] Task 6: 扩展调度器 (sched.c 修改)
  - [x] 任务结构新增 pid、user_rsp、entry 字段
  - [x] 实现 `sched_create_user_task()` 创建用户态任务
  - [~] 实现用户态→内核态切换逻辑 (W6 实现 — W5 使用 Ring 0)

- [x] Task 7: 扩展 Shell (shell.c 修改)
  - [x] 新增 `run <path>` 命令 (加载并执行 ELF)
  - [x] 新增 `ls <path>` 命令 (列出目录内容)
  - [x] 新增 `cat <path>` 命令 (显示文件内容)

- [x] Task 8: 修改 kmain.c 注册新模块
  - [x] 调用 `syscall_init()` 初始化系统调用
  - [x] 调用 `vfs_init()` 初始化文件系统
  - [~] 调用 `elf_init()` 初始化 ELF 加载器 (ELF 无需显式初始化)

- [x] Task 9: 更新构建系统
  - [x] 修改 `build.ps1` 编译 `syscall/`, `elf/`, `fs/` 文件
  - [x] 新增 include 路径

- [x] Task 10: 构建、测试与验收
  - [x] 构建内核，确保 < 80KB (74,872 bytes)
  - [~] 测试 syscall 入口: sys_exit(0) 正常终止 (需 ELF 测试文件)
  - [~] 测试 ELF 加载: 解析 /bin/bash 成功 (需 ELF 测试文件)
  - [~] 测试 run 命令: run /bin/hello 输出 "Hello, world!" (需 ELF 测试文件)
  - [~] 测试 bash 运行: run /bin/bash 显示提示符 (需 ELF 测试文件)
  - [~] 测试错误处理: run /nonexistent 显示 File not found (需 ELF 测试文件)
  - [~] 测试权限校验: run /dev/null 显示 Not a valid executable (需 ELF 测试文件)

# Task Dependencies
- Task 2 依赖 Task 1 (syscall 框架先就绪)
- Task 3 依赖 Task 4 (ELF 加载需要 VFS 打开文件)
- Task 5 依赖 Task 3 (mmap 需要知道 ELF 段大小)
- Task 6 依赖 Task 3 (用户任务需要 ELF entry)
- Task 7 依赖 Task 3, 6 (run 命令需要 ELF 加载 + 任务创建)
- Task 8 依赖 Task 1, 4 (注册 syscall + VFS)
- Task 9 依赖 Task 1-3 (所有源文件就绪)
- Task 10 依赖 Task 1-9