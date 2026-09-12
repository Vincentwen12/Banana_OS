# Checklist

- [ ] python / python3 命令可用（`python -V` 输出与 `python3 -V` 一致，无 command not found）
- [ ] bash 无 `turning off output flushing` 警告，输入即时回显、回车后提示符正确重绘
- [ ] bash 启动/首次输入无整屏补全候选 spam
- [ ] 工具命令（vim / gcc / make / tar / python3）运行后 bash 存活并回到提示符，可连续输入，无内核 panic / 无软重启 / 不回内核 Shell
- [ ] 回归：`ls`/`cd`/`cat` 相对路径正常，`test_w7_fsimg.ps1` 全绿
- [ ] kernel.flat < 80KB 约束保持，无 [dbg] 打印残留
