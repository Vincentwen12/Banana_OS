# Tasks: W6 — 真实文件系统 + ELF 加载 + 用户态切换

## 阶段一：存储与文件系统

- [x] Task 1: 实现 ATA PIO 块设备驱动
  - [x] 创建 `src/kernel/dev/ata.h`（`ATA_PRIMARY_IO=0x1F0`、`ATA_PRIMARY_CTRL=0x3F6`、`ATA_SECTOR_SIZE=512`）
  - [x] 创建 `src/kernel/dev/ata.c`，实现 `ata_init()` 检测主通道设备
  - [x] 实现 `ata_read_sector()`（28-bit LBA + PIO 轮询，返回 0 成功 / 负值超时）
  - [x] 实现 `ata_write_sector()` 与 `ata_read_sectors()`
  - [x] 在 `kmain.c` 调用 `ata_read_sector(0, buf)` 验证 boot 签名 `0x55AA`（EXT2 魔数 `0xEF53` 验证留待 Task 3 挂载）

- [ ] Task 2: 准备 EXT2 测试盘
  - [ ] 创建 8MB raw 镜像（qemu-img 或预生成 ext2 镜像）
  - [ ] 用 mkfs.ext2 格式化并复制测试文件（`/bin/bash`、`/lib/ld-linux-x86-64.so.2`、`/etc/hostname` 等）
  - [ ] 修改 `build.ps1`/测试脚本，将 ext2 镜像作为第二个 IDE 盘挂载到 QEMU

- [ ] Task 3: 实现 EXT2 只读驱动（超级块 + 组描述符）
  - [ ] 创建 `src/kernel/fs/ext2.h`（superblock/group_desc/inode/dir_entry 结构）
  - [ ] 创建 `src/kernel/fs/ext2.c`，实现 `ext2_mount()` 读取超级块与组描述符
  - [ ] 打印块大小、inode 数、总块数（验收：无错误）

- [ ] Task 4: 实现 EXT2 inode 与目录读取
  - [ ] 实现 `ext2_read_inode()`（含 inode 位图/inode 表定位）
  - [ ] 实现 `ext2_read_dir()`（变长目录项遍历，回调 name+ino）
  - [ ] 实现 `ext2_lookup()`（按名称解析子 inode）
  - [ ] 实现 `ext2_read_file()` 与符号链接解析
  - [ ] 验收：`ls /` 列出 `/bin` `/lib` `/etc` 等

## 阶段二：VFS 路径解析

- [ ] Task 5: 增强 VFS 与文件对象
  - [ ] 扩展 `fs/vfs.h` 的 `file` 结构（新增 `refcount`）
  - [ ] 创建 `fs/file.c/h`（文件描述符表，dup/close 真实支持）
  - [ ] 重写 `vfs_open()` 为真实路径解析（绝对路径逐级 ext2_lookup）
  - [ ] 实现 `vfs_mount()` 挂载点管理
  - [ ] 验收：`vfs_open("/bin/bash")` 返回指向磁盘 inode 的 file

## 阶段三：用户态页表与切换

- [ ] Task 6: 实现用户态页表管理
  - [ ] 创建 `src/kernel/mm/vm.h`（`vm_area_t`、`mm_context_t`）
  - [ ] 创建 `src/kernel/mm/vm.c`：`vm_init`、`vm_map`（U/S=1）、`vm_unmap`、`vm_switch`
  - [ ] 修改 `mm.c` 的 `mmap_user` 委托 `vm_map` 并使用任务 `mm_context`
  - [ ] 验收：单独测试 `vm_switch`，逐页打印页表条目，无 #PF

- [ ] Task 7: 实现用户态切换（Ring 3）
  - [ ] 创建 `src/kernel/sched/switch.S`：`switch_to_user`（构建 iretq 栈帧）
  - [ ] 新增 GDT 段：用户代码段（0x18）、用户数据段（0x20）
  - [ ] 配置 EFER.SCE 与 STAR MSR 供 `syscall` 使用
  - [ ] 修改 `sched.c` 的 `task_t` 新增 `mm_context` 字段
  - [ ] 验收：用户程序在 Ring 3 执行，syscall 可往返

## 阶段四：ELF 与 execve

- [ ] Task 8: 重构 ELF 加载器
  - [ ] 修改 `elf/loader.c` 使用 `vm_map` 映射 `PT_LOAD` 段（处理非页对齐文件偏移）
  - [ ] 识别 `PT_INTERP`（记录解释器路径），处理 `PT_GNU_STACK`
  - [ ] 分配 4MB 用户栈，设置 entry
  - [ ] 修改 `elf_load()` 签名返回 entry/stack_top/interp_path

- [ ] Task 9: 实现动态链接器加载
  - [ ] 修改 `elf/interp.c` 加载 `/lib/ld-linux-x86-64.so.2`（fallback `/lib64/...`）
  - [ ] 解释器 entry 成为实际入口，程序头地址经用户栈传递
  - [ ] 验收：`readelf -d /bin/bash` 显示需要动态链接器

- [ ] Task 10: 实现完整 execve 与补充系统调用
  - [ ] 修改 `syscall/table.c` 实现 `sys_execve` 完整路径（open → elf_load → 压栈 → switch_to_user）
  - [ ] 实现 `fork`（复制任务与页表，暂不 COW）、`wait4`、`kill`(SIGKILL)
  - [ ] 实现空实现：`rt_sigaction`、`rt_sigprocmask`、`rt_sigreturn`、`chdir`
  - [ ] 实现 `fcntl`(F_DUPFD/F_GETFD)、`getcwd`(返回 "/")
  - [ ] 准备用户栈（argc/argv/envp）

## 阶段五：集成与验收

- [ ] Task 11: 集成初始化与构建
  - [ ] 修改 `kmain.c` 初始化 ata/ext2/vfs/vm，挂载 `/dev/sda` 到 `/`
  - [ ] 修改 `build.ps1` 编译新增模块（dev/、fs/ext2.c、fs/file.c、mm/vm.c、sched/switch.S）
  - [ ] 移除 tmpfs 硬编码 ELF 注册路径（`hello_init.c` 改用磁盘 `/bin/bash`）

- [ ] Task 12: 端到端验收
  - [ ] `run /bin/bash` 进入 `bash-5.2$` 提示符
  - [ ] bash 内执行 `echo "ok"` 输出 ok
  - [ ] bash 内执行 `/bin/ls` 显示文件列表
  - [ ] bash 内执行 `exit` 返回 Shell
  - [ ] `run /nonexistent` 显示 File not found
  - [ ] 内核 `< 80 KB`，启动 `< 2s`

# Task Dependencies
- Task 2 依赖 Task 1（测试盘依赖块设备驱动）
- Task 4 依赖 Task 3（inode/目录依赖超级块/组描述符）
- Task 5 依赖 Task 3, 4（路径解析依赖 EXT2 解析）
- Task 8 依赖 Task 6（ELF 加载依赖 vm_map）
- Task 3, 6 互为独立，可并行
- Task 5, 6 独立，可并行
- Task 9 依赖 Task 8（动态链接依赖 ELF 重构）
- Task 10 依赖 Task 5, 7, 9（execve 依赖 VFS + 用户态切换 + 动态链接）
- Task 11 依赖 Task 1-10
- Task 12 依赖 Task 11