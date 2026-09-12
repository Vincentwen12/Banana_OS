# Checklist: Axion-Ban Epoch v4.0

## W1: 闪电启动
- [x] `make run` 在 QEMU 中成功启动，无 triple fault
- [x] 串口输出 "BananaOS Axion-Ban Epoch v4.0" 启动 Banner
- [x] 串口输出启动耗时（毫秒）
- [x] 启动到 `epoch> ` 提示符总时间 < 2 秒
- [x] `help` 命令列出所有可用命令
- [x] `clear` 命令清空屏幕
- [x] `reboot` 命令成功复位系统
- [x] `echo hello` 正确回显 "hello"
- [x] `mem` 命令显示内存统计（即使为占位数据）
- [x] 键盘输入字符正确回显到 VGA 屏幕
- [x] 退格键正确删除字符
- [x] 回车键正确提交命令
- [ ] 连续快速按 100 次键，所有字符显示正确
- [x] 未知命令显示 "Unknown command" 提示
- [x] 内核二进制 < 80 KB（实际 4.5KB）

## W2: 弹性内存
- [ ] `mm_alloc(1)` 返回有效物理地址
- [ ] `mm_alloc(0)` 或超大请求返回 NULL
- [ ] `mm_free()` 正确释放内存，后续可重新分配
- [ ] `mem` 命令显示 Total/Used/Free 数值正确
- [ ] 内存耗尽时系统不崩溃，`mm_alloc` 返回 NULL
- [ ] SWAR 热度计数器正确初始化和老化
- [ ] XOR 压缩/解压 4KB 页面数据正确（往返一致）

## W3: 调度 + IPC
- [ ] 调度器正确维护 5 级优先级队列
- [ ] 后台 CPU 满载时，前台 Shell 输入延迟 < 10ms
- [ ] 低优先级任务经老化后获得 CPU 时间
- [ ] 门铃 ring/wait 正确传递消息
- [ ] 门铃 ACL 拒绝未授权通道写入（返回 -EPERM）
- [ ] ELF 加载器正确解析并加载静态链接的 hello.elf
- [ ] `run hello.elf` 正确输出 "hello"
- [ ] 沙箱标签注入到加载的 ELF 任务中

## W4: 功耗 + 安全
- [ ] 空载时 CPU 执行 HLT，QEMU 主机侧 CPU < 5%
- [ ] 低负载时切换为轮询模式
- [ ] 高负载时切换为中断模式
- [ ] 沙箱程序访问未授权文件返回 -EACCES
- [ ] 沙箱程序写入系统保留门铃通道返回 -EPERM
- [ ] 系统调用 write/read/exit 基础功能可用
- [ ] fork 创建子进程，父子并行输出不同内容