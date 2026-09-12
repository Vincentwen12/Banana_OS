# Tasks

- [ ] Task 1: 复现并定位工具命令崩溃/退出根因
  - [ ] 1.1 运行 vim（交互式 full-screen）、python3 REPL、gcc -v、make、tar 逐一复现"必现重启/退出"
  - [ ] 1.2 观察现象分类：a) bash 自身退出（`calling fini: /bin/bash` 后回内核 Shell，`[TEST] bash exited`） vs b) 内核 panic（`[PANIC]` + 5 秒倒计时 + 0xCF9 软重启）
  - [ ] 1.3 若为 bash 退出：检查工具命令是否破坏 stdin/tty 状态（fd 0/1/2 被 dup2/close 改动、tty c_lflag 混乱、EOF 触发 bash exit）→ 修复 devfs/tty 或 fd 语义
  - [ ] 1.4 若为内核 panic：给 exec/syscall/mmap/fork 路径加最小 [dbg] 打印，定位崩溃 syscall 与参数 → 修复
  - [ ] 1.5 验证：工具命令运行后 bash 存活、回提示符、可连续输入，无 panic/重启

- [ ] Task 2: python 等常用命令可用
  - [ ] 2.1 `tools/rootfs/home/banana/.bashrc` 添加别名：`alias python=python3`、`alias ll='ls -l'`、`alias la='ls -a'`（附中文注释时用 UTF-8 无 BOM 且注释保持 ASCII 或 GBK 安全，历史坑：PowerShell 5.1 按 ANSI 解析）
  - [ ] 2.2 评估是否需要真实符号链接：若需要 `python -> python3`，扩展 `tools/ext2_mkfs.py` manifest 支持 symlink 行（`create_symlink` 已有）或在 fs_manifest 用专用标记；否则以别名方案交付
  - [ ] 2.3 验证：`python -V` 与 `python3 -V` 输出一致

- [ ] Task 3: 修复 terminfo 注入，让 bash 退出 dumb 模式
  - [ ] 3.1 确认 fs 镜像中 `lib/terminfo/x/xterm`、`lib/terminfo/d/dumb` 等条目真实存在且内容有效（`tools/rootfs/lib/terminfo/x/xterm` 字节非空、magic 正确）；缺失则补注入
  - [ ] 3.2 从 bash 内验证：`ls /lib/terminfo/x/xterm` 可列出（排除 ext2 路径解析 bug）；确认 bash 不再打印 `turning off output flushing`，回显即时、提示符重绘正常
  - [ ] 3.3 若路径解析失败是内核问题（dentry/ext2 lookup），修复后回归

- [ ] Task 4: 抑制启动补全 spam
  - [ ] 4.1 注入 `tools/rootfs/etc/inputrc`（全局）与 `home/banana/.inputrc`：`set completion-query-items 100`、`set show-all-if-ambiguous off`、`set bell-style none`、`set enable-bracketed-paste off`（按实测效果取舍）
  - [ ] 4.2 验证：bash 启动/首次输入不再刷屏补全候选列表

- [ ] Task 5: 构建 + 端到端验证
  - [ ] 5.1 `build.ps1` 重建（含新 fs.img 注入）
  - [ ] 5.2 验证：`python -V`、回显/提示符、无补全 spam、工具命令（vim/gcc/make/tar/python3）运行后 bash 存活回提示符
  - [ ] 5.3 回归：`test_w7_fsimg.ps1` 全绿、`ls`/`cd`/`cat` 相对路径正常

# Task Dependencies
- Task 2/3/4 相互独立，可并行；Task 1 独立。
- Task 5 依赖 Task 1-4。
