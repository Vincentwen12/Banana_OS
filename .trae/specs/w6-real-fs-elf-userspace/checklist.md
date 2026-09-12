# Checklist: W6 — 真实文件系统 + ELF 加载 + 用户态切换

## 块设备层
- [x] `ata_init()` 检测到主通道设备
- [ ] `ata_read_sector(2, buf)` 返回 0，`buf[0:2] == 0xEF53`（需 EXT2 测试盘；已验证 sector 0 读 0x55AA 通过）
- [x] 超时/未就绪返回负值，不挂死（`ata_wait_ready`/`ata_wait_drq` 实现超时返回 -1）

## EXT2 只读驱动
- [ ] `ext2_mount()` 打印块大小、inode 数，无错误
- [ ] `ext2_read_inode(2)` 读取根目录 inode 成功
- [ ] `ext2_read_dir(2, cb)` 列出 `/bin` `/lib` `/etc` 等
- [ ] `ext2_lookup()` 按名称解析正确
- [ ] 符号链接解析正确（快速 + 慢速）

## VFS
- [ ] `vfs_open("/bin/bash", O_RDONLY)` 返回指向磁盘 inode 的 file
- [ ] `vfs_open("/nonexistent")` 返回 NULL
- [ ] `vfs_mount` 挂载 `/dev/sda` 到 `/`
- [ ] 文件描述符表支持 dup/close

## 用户态页表
- [ ] 每个任务独立页表（cr3）
- [ ] `vm_map` 建立 U/S=1 用户映射
- [ ] `vm_switch` 切换地址空间无 #PF
- [ ] 用户态不可访问内核空间

## 用户态切换
- [ ] GDT 含用户代码段 0x18、用户数据段 0x20
- [ ] `switch_to_user` 通过 iretq 进入 Ring 3
- [ ] `syscall` 指令往返成功（EFER.SCE + STAR 已配置）

## ELF 加载器
- [ ] `PT_LOAD` 段通过 `vm_map` 映射（处理非页对齐）
- [ ] `PT_INTERP` 被识别并返回解释器路径
- [ ] 4MB 用户栈分配正确

## 动态链接
- [ ] 加载 `/lib/ld-linux-x86-64.so.2`（含 fallback）
- [ ] `readelf -d /bin/bash` 显示需要 `ld-linux.so.2`

## execve 与系统调用
- [ ] `execve` 完整路径：open → load → 压栈 → switch_to_user
- [ ] 用户栈含 argc/argv/envp（遵循 x86-64 ABI）
- [ ] `fork` 复制任务与页表
- [ ] `wait4` 阻塞等待子进程
- [ ] `kill`(SIGKILL) 可终止进程
- [ ] `fcntl`/`getcwd`/`chdir`/`rt_sig*` 基础实现可用

## 端到端验收
- [ ] `run /bin/bash` 进入 `bash-5.2$` 提示符
- [ ] bash 内 `echo "ok"` 输出 ok
- [ ] bash 内 `/bin/ls` 显示文件列表
- [ ] bash 内 `exit` 返回 Shell
- [ ] `run /nonexistent` 显示 File not found
- [ ] 内核二进制 `< 80 KB`，启动 `< 2s`