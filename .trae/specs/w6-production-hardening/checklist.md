# Checklist: Axion-Ban 生产级强化（W6 细化）

按阶段组织；每项通过后勾选。

## 阶段 0：体积预算放宽
- [x] bootsect 以 LBA（int 13h AH=42h）加载 `KERNEL_SECTORS=1024` 扇区成功，无磁盘错误
- [x] 构造 >80KB 的 kernel.flat 可完整引导进入内核
- [x] 常规构建（当前 ~115KB）引导无回归；`kernel.flat < 512KB`
- [x] `build.ps1` 的 `minKernelSectors` 与 `disk.img` 布局与 bootsect 一致

## 阶段 1：安全基础

### 内存隔离
- [x] `vm_create()` 的 0-2GB identity 大页 U/S=0（无 `PTE_USER`）
- [x] `vm_map_page()` 拆分大页时仅目标页 U/S=1，兄弟页 U/S=0
- [x] 用户程序访问 `0x100000`（内核代码）触发用户态 #PF
- [x] 用户程序访问未映射高地址（如 `0xFFFF800000000000`）触发用户态 #PF
- [x] 用户程序正常内存（ELF/栈/brk/mmap）访问无异常
- [x] 门铃 IPC / 共享区域（`ipc` 命令）回归通过

### 用户态异常终止进程
- [x] 用户态 #GP/#PF/#UD → 仅该进程终止，内核与 Shell 存活
- [x] 子进程崩溃时父进程 `wait4` 返回非 0 状态，bash 继续
- [x] 内核态异常 → 打印现场并走 panic（非整机静默重启）

### Panic 恢复
- [x] `panic` 命令输出栈回溯 + 进程名/PID + 寄存器快照
- [x] 5 秒后经 `0xCF9` 自动重启
- [x] 重启后显示 "Previous panic: ..."
- [x] 连续 3 次 panic 进入仅内核 Shell 的安全模式

### EXT2 写入
- [x] 分配/释放 inode 与块后位图与组描述符回写一致（重启核对）
- [x] 直接改写已有文件数据块，重启后读到新内容
- [x] `echo "hello" > /tmp/test.txt` 创建文件并写入
- [x] `cat /tmp/test.txt` 显示 `hello`
- [x] `rm /tmp/test.txt` 删除文件；`cat` 报不存在
- [x] **重启后文件/删除均持久化**

## 阶段 2：日常可用

### printk / dmesg
- [x] `dmesg` 显示最近 ≥64 条内核消息（含级别）
- [x] panic 后日志缓冲保留；写入磁盘后重启不丢
- [x] 启动/异常关键路径接入 printk

### /proc 与 ps
- [x] `ps` 列出进程（PID、名称、状态）
- [x] `cat /proc/self/status` 返回当前进程信息
- [x] `cat /proc/uptime` 返回运行时间

### /sys
- [x] `cat /sys/kernel/version` 返回版本字符串

### 多用户基础
- [x] uid/gid 系统调用返回真实值（root=0）
- [x] 权限位为 0 的文件被非 owner 打开返回 EACCES

### 信号完整性
- [x] `rt_sigaction` 保存/读取 handler 表
- [x] `kill` 发送 SIGTERM/SIGINT 等默认动作生效；SIGKILL 强制终止
- [x] 注册 handler 的进程收到信号后执行 handler
- [x] 用户态异常（#PF）对注册了 SIGSEGV handler 的进程投递而非直接终止

## 阶段 3：生产增强

### 终端控制
- [x] termios：ICANON 行缓冲、回显、VMIN/VTIME 生效
- [x] `tcsetpgrp`/`TIOCSPGRP` 真实生效，前台进程组跟踪
- [x] `sleep 2 &` 后台运行、`jobs` 列出、`fg` 收回前台
- [x] 无 "no job control" 警告
- 注：回显为无条件即时回显（readline dumb 模式历史教训，非按 ECHO 位）；`&`/`fg`/`bg` 为同步模型最小语义（无真并行后台）

### IPC
- [x] 两进程经共享内存交换数据成功
- [x] 信号量/消息队列（或等价）基础可用

### 网络（loopback）
- [x] 进程 A `socket/bind/listen/accept`（127.0.0.1），进程 B `connect/send/recv` 成功
- [x] 双向收发字符串一致

### 磁盘写入优化
- [x] 连续写入 ≥1MB 数据无丢失，重启后校验一致

## 端到端回归
- [x] `run /bin/bash` 进入 bash；`echo hello`/`help` 可用
- [x] 快速打字即时回显（本会话已修复项无回归）
- [x] 外部命令（fork/execve/wait4）路径无重启
- [x] 多次冷启动无早期随机崩溃（IDT 无异常输出）
- [x] `kernel.flat < 512KB`；启动 < 2s
