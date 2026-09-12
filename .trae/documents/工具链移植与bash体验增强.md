# 工具链移植 / Python 库增强 / bash 体验与启动优化

## 摘要

用户需求四块：① 让更多 Python 标准库扩展（.so）/加密/科学计算库可用；② 移植更多编程应用（其它语言运行时 + 编译工具链 + Python 库增强）；③ 优化性能与 shell 体验；④ 干净启动直接进 bash，且 bash 退出后自动重启（登录循环式）。

现状：内核 Linux ABI 已覆盖 ~150 个 syscall（[table.c](file:///d:\BananaOS-Axion\src\kernel\syscall\table.c) 2315-2424 行），bash/python3/vim/gcc/make/tar 已能在 bash 下稳定运行（test_shell_ux 9/9 全绿）；fs.img 512M 已用 ~390M；45 个 Python 标准库扩展 .so 已在盘（[fs_manifest.txt](file:///d:\BananaOS-Axion\tools\fs_manifest.txt)），能否 import 取决于依赖 .so 是否在盘、所需 syscall、/dev 设备、证书等。

本计划按「高价值低风险 → 诊断驱动 → 可行性优先」排序为 A/B/C/D 四阶段，每阶段独立验收，均可单独交付。

## 阶段 A：干净启动直接进 bash + bash 退出自动重启（先做）

改动文件：[kmain.c](file:///d:\BananaOS-Axion\src\kernel\kmain.c)

现状：boot 后打印 `[TEST] Auto-running 'run /bin/bash'...`；bash 退出后打印 `[TEST] bash exited, back at kernel shell.` 并回内核 Shell（shell_activated 一次性标志，kmain 主循环 368-376 行）。

改动：
1. **去测试日志**：`[TEST] Auto-running` → 无前缀或 `[BOOT] Starting bash...`；`[TEST] bash exited` 移除。Safe mode（连续 panic ≥3）保留进内核 Shell 逻辑。
2. **bash 退出自动重启**：主循环 368-376 行改为——当 g_fg_pid 对应 bash 退出后，短暂清理（sched 已自动回收），重新 `shell_launch_program("/bin/bash", 0, NULL)` 启动新 bash，形成登录循环（Ubuntu 行为）。仅当 bash **连续启动失败 N 次**（防崩溃风暴）才回落内核 Shell。
3. 内核 Shell 仍保留（用于 safe mode / 诊断），但正常流程不再自动进入。

完成定义：boot 后无 [TEST] 字样直接进 bash；在 bash 内 `exit` 或 Ctrl-D 后**自动重启进入新 bash**（出现新提示符）；连续 panic 进内核 Shell 兜底。

## 阶段 B：Python 库诊断与修复（诊断驱动）

### B1 诊断
写 `test_pyimport.ps1`：启动 bash → `python3 -c` 逐个 import 45 个 .so 对应的模块（`_ssl,ssl, _hashlib,hashlib, _sqlite3,sqlite3, _curses,curses, _ctypes,ctypes, _decimal,decimal, _bz2,bz2, _lzma,lzma, _crypt,crypt, mmap, resource, termios, readline, _uuid,uuid, _zoneinfo,zoneinfo, _asyncio,asyncio, _multiprocessing, _posixshmem, _queue, audioop`），记录每个：OK / ImportError 的具体原因。

### B2 按错误分类修复（修复进 fs 内容为主，内核改动最小化）
- **缺依赖 .so（ImportError: libxxx.so not found）**：`_ssl`→libssl3、`_sqlite3`→libsqlite3-0、`_curses`→libncursesw6、`_bz2`→libbz2、`_lzma`→liblzma5、`_crypt`→libcrypt1、`_ctypes`→libffi8、`readline`→libreadline8(已有 libtinfo6)、`_uuid`→libuuid1、`_decimal`→libmpdec3、`hashlib`→libssl 加密后端。逐一用 [fetch_deps.py](file:///d:\BananaOS-Axion\tools\fetch_deps.py) `--packages` 追加到 TOP_PACKAGES 下载注入。
- **缺设备/文件**：`/dev/urandom`(getrandom 已实现，但 os.urandom 可能走 /dev/urandom 文件——devfs 补 `/dev/urandom`、`/dev/random` 只读设备)；`hashlib` 需 `/etc/ssl` 或 openssl.cnf（导入时通常不需要，实测定）。
- **缺 syscall 返回 ENOSYS**：内核 stub 补齐（如 `getuid/geteuid` 已有；若诊断出现新缺项，注册返回合理默认的 stub，控制内核增量）。
- **numpy（科学计算，单独子项）**：追加 `python3-numpy` 包，实测 `import numpy`。注意 numpy wheel 依赖 libopenblas 等（体积大），若 fs 剩余空间不足（~120M）则评估裁剪或仅保证 import 成功（np.array/简单运算）。

### B3 回归
`python3 -c "import ssl,hashlib,sqlite3,ctypes,decimal,bz2,lzma,crypt,mmap,uuid,zoneinfo,termios,readline,asyncio;print('ALL-OK')"`。

完成定义：上述模块全部 import 成功（真联网/真实加密握手受无网卡限制，import+基本调用可用即可，并在文档注明限制）。

## 阶段 C：更多编程应用移植（可行性优先，逐个实测）

流程（每个候选）：`fetch_deps.py --packages "<pkg>"` → 解包注入 → 实测 `--version`/hello world → 记录缺的 syscall/文件 → 修复 → 交付。按依赖少到多排：

1. **perl**（`perl` + `libperl5.34`）：成熟、依赖少，作为第一个验证运行时。
2. **lua / ruby**（`lua5.3` / `ruby`）：轻量运行时；ruby 依赖较多需评估。
3. **node.js**（`nodejs`）：大，含自带 V8，评估 fs 空间与 syscall（epoll 缺失→node 事件循环可能退化；实测）。
4. **go 工具链**（`golang`）：自包含编译器，运行时依赖 clone/futex（已实现）；实测 `go run`/`go build`。
5. **python3 第三方增强**：numpy 见 B；如需 pip 生态则评估体积。

每个移植必须有实机测试通过才计入完成；不通过的记录阻塞原因作为已知限制（不强行塞入）。

## 阶段 D：性能优化 + shell 体验打磨

### 性能
- 测量基线：boot→bash 提示符耗时（现 ~217ms/smp4、745ms/smp1）；bash 内 `ls`/`python3 -c` 响应。
- 优化候选（实测后选收益高的）：bash 启动路径 syscall 减负；Python 首次 import 提速（预生成 `__pycache__`/`.pyc` 到 fs，跳过编译）；串口输出批量刷；调度器热点微调。
- 不做无收益改动。

### shell 体验
- 内核 Shell：已有历史(↑/↓)+Tab 补全；可补命令 `restart-bash`/`bash` 手动重启。
- bash 侧：注入 `PS1` 颜色提示符（root 红色/普通绿色风格）；`/etc/bash.bashrc` 加 `alias ll='ls -l'`、`alias la='ls -a'`、`alias ..='cd ..'`；确认 `export EDITOR=vim`。
- 终端：验证 Ctrl-C 中断前台程序、Ctrl-D EOF（配合阶段 A 自动重启）。

## 假设与决策

- 网络栈缺失为已知硬限制：`ssl` 联网、`socket` 实际通信不可用，但 import 与本地 API 可用；不实现网卡驱动（超出本轮）。
- 内核增量最小化：能靠 fs 内容解决的（.so、证书、配置）不动内核；必须补 syscall/设备的改动逐个评估并控制体积。
- bash 自动重启用「连续失败 N 次回退内核 Shell」防崩溃风暴，N=3。
- 80KB 内核约束当前已被历史累积超出（140KB），本轮不为移植额外大幅增内核。

## 验证

1. 阶段 A：手动 boot → 直接进 bash 无 [TEST]；`exit` 后自动重启 bash；连续 panic 回内核 Shell。
2. 阶段 B：import 全清单脚本全 OK；test_shell_ux.ps1 回归全绿。
3. 阶段 C：每个新增运行时的 `--version` + hello world 实测通过。
4. 阶段 D：boot→bash 时间、常用命令响应测量对比记录；alias/PS1 生效。
