# 工具链崩溃与 Shell 手感修复 Spec

## Why

用户反馈三个问题：
1. `python` 命令报 `command not found`（fs 镜像只有 `python3`，无 `python` 符号链接/别名）。
2. 特定工具命令（vim/gcc/make/tar 类）必现"重启"——日志显示 `calling fini: /bin/bash`（bash 或其子进程退出），系统随后回内核 Shell 或内核 panic 软重启，用户感知为"一输命令就重启"。
3. Shell 手感差：bash 进入 readline dumb 模式（日志 `[lk] NOTFOUND p=95 n=x`/`p=3218 n=terminfo` 说明 terminfo 查找失败），回显延迟、提示符不刷新；启动期补全 spam 干扰输入。

## What Changes

- **用户空间（fs 镜像）**：`.bashrc` 添加 `python`/`ll`/`la` 等常用别名；注入 `python → python3` 符号链接或别名，消除 `command not found`。
- **终端手感**：修复 terminfo 注入使其对 bash 可用（`TERM=xterm` → `/lib/terminfo/x/xterm` 能解析），让 readline 退出 dumb 模式，回显/提示符即时刷新。
- **补全 spam**：注入 `/etc/inputrc` 或 `~/.inputrc`，抑制 bash 启动期一次性补全列表输出。
- **工具命令崩溃/退出（内核排查）**：复现特定工具命令必现的崩溃/退出，仪器化定位是"bash 退出（fd/tty 状态）"还是"内核 panic（syscall 缺陷）"，修复根因。

## Impact

- Affected specs: w6-real-fs-elf-userspace（真实工具链运行）、w5-linux-abi-compat（syscall 层）
- Affected code:
  - `tools/fs_manifest.txt`、`tools/rootfs/home/banana/.bashrc`、`tools/rootfs/etc/inputrc`（用户空间配置注入）
  - `tools/ext2_mkfs.py`（如需符号链接注入支持）
  - 内核：`src/kernel/syscall/`、`src/kernel/fs/devfs.c`、`src/kernel/fs/vfs.c`、`src/kernel/sched/sched.c`（若崩溃根因在内核）
  - `build.ps1`（重建 fs.img 注入）

## ADDED Requirements

### Requirement: 常用命令可用（python 等）
系统 SHALL 让 `python` 命令可用并执行 `python3`。
- 用户输入 `python` → 输出与 `python3` 一致（版本/REPL 均可）
- 用户输入 `python3 -V` → 输出 `Python 3.10.x`

#### Scenario: python 别名生效
- **WHEN** 用户运行 `python`
- **THEN** 不再报 `command not found`，而是进入 python3 解释器或打印版本

### Requirement: 终端回显与提示符即时刷新
系统 SHALL 使 bash 退出 readline dumb 模式，输入即时回显、回车后提示符正确重绘。
- bash 启动不再出现 `readline: warning: turning off output flushing`
- bash 能解析 `TERM=xterm` 对应的 terminfo 条目（`ls /lib/terminfo/x/xterm` 存在且可读）
- 输入字符即时回显，无"按编辑键才显示"现象

#### Scenario: terminfo 生效
- **WHEN** bash 启动且 `TERM=xterm`
- **THEN** 无 dumb 模式警告，键入命令逐字符回显，回车后 `\u@\h:\w$` 提示符在原位刷新

### Requirement: 抑制启动补全 spam
系统 SHALL 在 bash 启动/首次输入时不输出整屏补全候选列表（`Display all N possibilities? (y or n)` 及数百行候选）。

#### Scenario: 无补全 spam
- **WHEN** bash 启动后用户直接输入命令
- **THEN** 终端只回显输入与命令输出，无补全候选列表刷屏

### Requirement: 工具命令运行后 bash 存活且系统不重启
系统 SHALL 让 vim / gcc / make / tar / python3 等工具命令正常运行、退出，随后 bash 保持存活并回到提示符，系统不出现内核 panic 软重启、不回内核 Shell。

#### Scenario: 工具命令正常返回
- **WHEN** 用户运行某工具命令（如 `vim`、`gcc -v`、`make`、`tar`）
- **THEN** 命令完成并退出，bash 回到 `banana@axion-ba:...$` 提示符，可继续输入

## MODIFIED Requirements

### Requirement: 既有工具链冒烟（test_w7_fsimg）
`ls /bin/bash /usr/bin/python3 ...`、`python3 -V`、`vim`、`gcc`、`make` 等既有冒烟用例 SHALL 保持通过，不受本改动影响。

## REMOVED Requirements

无。
