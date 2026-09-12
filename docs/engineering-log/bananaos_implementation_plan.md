# BananaOS 实施计划 (基于 Axion 架构 v1.1)

## 1. 摘要

设计并实现 **BananaOS** —— 一个确定性实时操作系统内核原型，运行于 x86-64 裸机。核心价值：消除中断、调度、虚拟内存、内核栈等一切不确定性来源，换取极低且可预测的最坏情况延迟。

**三大设计原则：**
- 无虚拟内存（CR0.PG=0，全系统物理地址线性映射）
- 无中断（禁用 PIC/APIC，全部 I/O 轮询）
- 无内核栈（禁止函数嵌套调用，全部逻辑以无栈状态机实现）

## 2. 当前状态分析

### 已有资源
```
d:\BananaOS-Axion\
└── x86_64-elf/                    # 交叉编译工具链
    ├── bin/
    │   ├── x86_64-elf-gcc (未确认，但常用)
    │   ├── as.exe                 # GNU Assembler
    │   ├── ld.exe / ld.bfd.exe   # GNU Linker (BFD)
    │   ├── objcopy.exe            # 用于生成 binary/ISO
    │   ├── objdump.exe            # 反汇编
    │   ├── readelf.exe            # ELF 分析
    │   ├── ar.exe                 # 归档
    │   ├── nm.exe                 # 符号表
    │   ├── ranlib.exe             # 归档索引
    │   └── strip.exe              # 符号剥离
    └── lib/
        └── ldscripts/             # 链接脚本模板 (elf_x86_64.x 等)
```

### 外部依赖
- **QEMU**: `D:/qemu` (用于模拟运行)
- **操作系统**: Windows, PowerShell 环境

### 缺失项
- 所有源代码文件（.S, .c, .h）
- 链接脚本 (linker.ld)
- 构建系统 (Makefile)
- GRUB/ISO 引导配置
- 任何内核代码

## 3. 项目目录结构设计

```
d:\BananaOS-Axion\
├── x86_64-elf/                    # 交叉编译工具链 (已有)
├── src/
│   ├── boot/
│   │   └── boot.S                 # Multiboot 引导头 + 长模式切换
│   ├── kernel/
│   │   ├── kmain.c                # 内核主入口
│   │   ├── vga.c / vga.h          # VGA 文本模式输出
│   │   ├── port.c / port.h        # I/O 端口操作 (inb/outb)
│   │   ├── mm/
│   │   │   ├── pmalloc.c / pmalloc.h  # 物理内存分配器 (线性堆)
│   │   │   └── srat.c / srat.h        # ACPI SRAT 解析
│   │   ├── ipc/
│   │   │   ├── doorbell.c / doorbell.h  # 门铃机制 (核间通信)
│   │   │   └── ringbuf.c / ringbuf.h    # 环形缓冲区
│   │   ├── task/
│   │   │   └── statemachine.h          # 无栈状态机框架
│   │   ├── io/
│   │   │   ├── keyboard.c / keyboard.h # PS/2 键盘轮询
│   │   │   └── shell.c / shell.h       # 无栈 Shell 状态机
│   │   ├── smp/
│   │   │   ├── ap.c / ap.h             # AP 启动 (INIT-SIPI)
│   │   │   └── watchdog.c / watchdog.h  # 看门狗
│   │   └── power/
│   │       └── idle.c / idle.h          # MONITOR/MWAIT 退避
│   └── include/
│       └── bananaos.h                 # 全局类型定义与常量
├── linker.ld                       # 自定义链接脚本
├── Makefile                        # 构建系统
├── iso/
│   └── grub/
│       └── grub.cfg                # GRUB 引导配置
└── .trae/
    └── documents/
        └── bananaos_implementation_plan.md  # 本文档
```

## 4. 分阶段实施计划

### W1: 裸机引导与长模式（无分页）

**目标**: 可启动 ISO，屏幕显示 "BananaOS v1.1"

#### 4.1.1 创建链接脚本 (`linker.ld`)
- 定义 Multiboot 兼容的 ELF 布局
- 入口点 `_start`
- 段定义: `.multiboot`, `.text`, `.rodata`, `.data`, `.bss`
- 起始地址: 物理地址 `0x100000` (1MB)
- 对齐: 4KB 页对齐（即使不使用分页也保持兼容性）

#### 4.1.2 创建引导汇编 (`src/boot/boot.S`)
- **Multiboot 头**: magic=0x1BADB002, flags=0x00000003, checksum
- **32位保护模式入口** `_start`:
  - 检查 Multiboot magic (eax=0x2BADB002)
  - 栈指针设为 `stack_top` (BSS 中预分配)
  - 检查 CPUID 是否支持长模式
  - 检查 MSR 是否支持 (CPUID 0x80000001:EDX bit 29)
  - 设置 4 级页表（仅 2MB 巨型页映射，最低需求）:
    - PML4[0] → PDPT (映射前 512GB)
    - PDPT[0] → 首个 1GB 的 2MB 页表
    - 实际上只需映射前 2MB (identity map)
  - 设置 IA32_EFER.LME=1 (bit 8)
  - 设置 CR4.PAE=1 (bit 5)
  - 加载页表地址到 CR3
  - 设置 CR0.PG=1, CR0.PE=1
  - 远跳转到 64 位代码段 `long_mode_start`
- **64位模式入口** `long_mode_start`:
  - 重新加载各段寄存器 (DS, ES, FS, GS, SS 设为 0x00)
  - 设置栈指针 (RSP)
  - 调用 `kmain()` (C 函数)
  - 挂起: `hlt` 循环

#### 4.1.3 创建内核入口 (`src/kernel/kmain.c`)
- 实现 `kmain()` 函数
- 调用 `vga_init()` 初始化 VGA
- 调用 `vga_puts("BananaOS v1.1\n")` 输出欢迎信息
- 调用 `vga_puts("Axion Architecture - Deterministic RTOS\n")`
- 进入无限循环 (后续替换为主循环)

#### 4.1.4 创建 VGA 驱动 (`src/vga.c` + `src/vga.h`)
- 物理地址 `0xB8000` 映射
- 80x25 字符模式
- 函数:
  - `vga_init()`: 清屏
  - `vga_putc(char c)`: 输出单个字符，处理 `\n` 换行
  - `vga_puts(const char* s)`: 输出字符串
  - `vga_clear()`: 清屏
- 内部维护 cursor_x, cursor_y, color

#### 4.1.5 创建 I/O 端口工具 (`src/port.c` + `src/port.h`)
- `static inline uint8_t inb(uint16_t port)`
- `static inline void outb(uint16_t port, uint8_t val)`
- `static inline uint16_t inw(uint16_t port)`
- `static inline void outw(uint16_t port, uint16_t val)`
- `static inline uint32_t ind(uint16_t port)`
- `static inline void outd(uint16_t port, uint32_t val)`
- 全部使用内联汇编

#### 4.1.6 创建全局头文件 (`src/include/bananaos.h`)
- 基础类型定义: `uint8_t`, `uint16_t`, `uint32_t`, `uint64_t`, `int32_t`, `size_t`, `bool`, `NULL`, `true`, `false`
- 物理地址常量: `VGA_BASE`, `KERNEL_BASE` 等
- 不依赖标准库

#### 4.1.7 创建 Makefile
- 目标:
  - `make all`: 构建 kernel.bin + ISO
  - `make iso`: 生成 bananaos.iso
  - `make run`: 用 QEMU 运行
  - `make clean`: 清理构建产物
- 编译器: `./x86_64-elf/bin/x86_64-elf-gcc` (如果存在) 或从 PATH 查找
- 汇编器: `./x86_64-elf/bin/as`
- 链接器: `./x86_64-elf/bin/ld`
- 标志:
  - ASFLAGS: `--64`
  - CFLAGS: `-m64 -mno-red-zone -nostdlib -ffreestanding -fno-builtin -Wall -Wextra -O2 -mcmodel=kernel`
  - LDFLAGS: `-T linker.ld -nostdlib`

#### 4.1.8 创建 GRUB 配置 (`iso/grub/grub.cfg`)
- 设置超时 0 秒
- 菜单入口指向 `/boot/kernel.bin`

#### 4.1.9 W1 验证
- `make run` 启动 QEMU
- 屏幕显示 "BananaOS v1.1" 和 "Axion Architecture - Deterministic RTOS"
- 无崩溃、无三重故障

---

### W2: 内存分配器与门铃框架

**目标**: 内核可分配物理内存，主核能轮询自身门铃

#### 4.2.1 禁用中断与 PIC/APIC (`src/kernel/kmain.c` 更新)
- 在 `kmain()` 入口处调用 `pic_disable()` 和 `apic_disable()`
- `pic_disable()`: 重新映射并屏蔽 8259 PIC (写 0xFF 到 0x21 和 0xA1)
- `apic_disable()`: 禁用 LAPIC (写 0x1B 到 MSR 0x1B 的 spurious vector 寄存器)
- 确认 `CLI` 已在引导阶段执行

#### 4.2.2 ACPI SRAT 表解析 (`src/mm/srat.c` + `srat.h`)
- 从 Multiboot 信息获取 ACPI RSDP 指针
- 解析 RSDT/XSDT 找到 SRAT 表 ("SRAT" 签名)
- 解析 SRAT 条目:
  - Type 0: 处理器本地 APIC/SAPIC 亲和性
  - Type 1: 内存亲和性
- 构建 `numa_node_t` 数组 (最多 4 个 NUMA 节点)
- 每个节点记录: node_id, 内存基址, 内存长度, 所属 APIC ID 列表

#### 4.2.3 线性堆分配器 (`src/mm/pmalloc.c` + `pmalloc.h`)
- 数据结构:
  ```c
  typedef struct {
      void*   heap_base;      // 堆起始物理地址
      void*   heap_ptr;       // 当前分配指针
      size_t  heap_size;      // 堆总大小
      size_t  allocated;      // 已分配字节数
      float   threshold;      // 压缩阈值 (0.8)
      // 双缓冲字段
      void*   compact_buffer; // 压缩缓冲区 (与 heap 等大)
      bool    compacting;     // 是否正在压缩中
  } heap_t;
  ```
- `pmalloc_init(int node_id, void* base, size_t size)`: 初始化堆
- `void* pmalloc(int node_id, size_t size)`: O(1) 分配，`heap_ptr += size` 并返回旧指针
- `void pfree(void* ptr)`: 空操作（记录释放，不回收，靠双缓冲压缩回收）
- 双缓冲压缩:
  - 当 `allocated / heap_size > 0.8` 时触发
  - 遍历所有存活对象，拷贝到 `compact_buffer`
  - 交换 `heap_base` 和 `compact_buffer` 角色
  - 重置 `heap_ptr` 和 `allocated`

#### 4.2.4 门铃机制 (`src/ipc/doorbell.c` + `doorbell.h`)
- 数据结构 (缓存行对齐，64 字节):
  ```c
  typedef struct __attribute__((aligned(64))) {
      volatile uint64_t flag;     // 门铃标志位
      uint64_t          padding[7]; // 填充到 64 字节
  } doorbell_t;
  ```
- `doorbell_ring(doorbell_t* db)`: 设置 `flag = 1`，写屏障
- `doorbell_poll(doorbell_t* db)`: 读取 `flag`，若为 1 则清零返回 true
- 门铃分配在固定物理页 (0x9000 - 0x9FFF，每个 64B 可容纳 64 个门铃)

#### 4.2.5 环形缓冲区 (`src/ipc/ringbuf.c` + `ringbuf.h`)
- 单生产者单消费者 (SPSC) 无锁实现
- 数据结构:
  ```c
  typedef struct {
      volatile uint64_t head;       // 生产者写入位置
      volatile uint64_t tail;       // 消费者读取位置
      uint64_t          size;       // 缓冲区大小 (2的幂)
      uint8_t*          buffer;     // 数据缓冲区
  } ringbuf_t;
  ```
- `ringbuf_init(ringbuf_t* rb, void* buf, uint64_t size)`
- `ringbuf_push(ringbuf_t* rb, const void* data, uint64_t len)`: 写数据
- `ringbuf_pop(ringbuf_t* rb, void* data, uint64_t len)`: 读数据

#### 4.2.6 W2 验证
- 内核启动后调用 `pmalloc()` 分配内存
- 打印各 NUMA 节点信息到 VGA
- 主核门铃自环测试（ring → poll 成功）

---

### W3: 无栈 Shell 与键盘轮询

**目标**: 交互式 Shell，键盘响应零中断延迟

#### 4.3.1 无栈状态机框架 (`src/task/statemachine.h`)
- 核心宏:
  ```c
  #define SM_BEGIN(sm)  switch((sm)->state) { case 0:
  #define SM_STATE(sm, n) case (n):
  #define SM_END(sm)    }
  #define SM_YIELD(sm, n) do { (sm)->state = (n); return; } while(0)
  ```
- 每个状态机对象包含 `uint32_t state` 和局部变量
- 主循环调用 `sm_dispatch(sm)` 执行当前状态

#### 4.3.2 PS/2 键盘轮询 (`src/io/keyboard.c` + `keyboard.h`)
- 状态机结构:
  ```c
  typedef struct {
      uint32_t state;
      uint8_t  scancode;
      char     ascii;
      bool     key_ready;
  } keyboard_sm_t;
  ```
- 轮询 0x64 端口 bit 0 检查是否有数据
- 读取 0x60 端口获取扫描码
- 扫描码集 1 到 ASCII 转换（仅支持字母数字和基本符号）
- 转换表: 静态数组，下标为扫描码，值为 ASCII 字符
- 状态机状态: `IDLE → READ → TRANSLATE → READY`

#### 4.3.3 Shell 状态机 (`src/io/shell.c` + `shell.h`)
- 状态机结构:
  ```c
  typedef struct {
      uint32_t state;
      char     cmd_buf[64];
      uint8_t  cmd_idx;
      char     prompt[16];
  } shell_sm_t;
  ```
- 状态: `PROMPT → READ_LINE → PARSE → EXECUTE → DONE`
- 命令:
  - `help`: 显示可用命令列表
  - `mem`: 显示各 NUMA 堆使用率
  - `ring <id>`: 测试门铃发送
  - `clear`: 清屏
  - `ver`: 显示版本 "BananaOS v1.1"
- 命令解析: 简单字符串匹配，无动态分配

#### 4.3.4 主循环集成 (`src/kernel/kmain.c` 更新)
- 替换 `while(1)` 为事件主循环:
  ```c
  void main_loop() {
      while (1) {
          // 1. 轮询键盘 → 更新 keyboard_sm
          keyboard_poll(&kbd);
          if (kbd.key_ready) {
              shell_input(&shell, kbd.ascii);
              kbd.key_ready = false;
          }
          // 2. 执行 shell 状态机
          shell_dispatch(&shell);
          // 3. 轮询门铃 (W4 扩展)
          // 4. 退避检查 (W4)
      }
  }
  ```

#### 4.3.5 W3 验证
- 启动后显示 `bananaos>` 提示符
- 输入 `help` 回显命令列表
- 输入 `mem` 显示内存使用率
- 输入 `ver` 显示版本
- 键盘按下到屏幕回显无感知延迟

---

### W4: 多核启动与稳定性加固

**目标**: 多核系统稳定运行，看门狗可复位无响应核心

#### 4.4.1 AP 启动序列 (`src/smp/ap.c` + `ap.h`)
- 使用 INIT-SIPI-SIPI 序列:
  1. 发送 INIT IPI (ICR 0x4500)
  2. 等待 10ms (使用 `port_delay()` 通过 0x80 端口)
  3. 发送 SIPI (ICR 0x4600 | trampoline_page)
  4. 等待 200μs
  5. 发送第二个 SIPI
  6. 等待 AP 标记门铃确认启动
- 跳板代码在物理地址 0x8000 (低于 1MB 实模式)
- 跳板代码:
  - 实模式 → 保护模式 → 长模式
  - 加载 CR3（复用 BSP 页表）
  - 跳转到 `ap_entry(apic_id)`

#### 4.4.2 AP 入口 (`src/smp/ap.c`)
- `ap_entry(int apic_id)`:
  - 初始化本地 GDT（复用 BSP GDT 但加载到独立位置）
  - 设置 IDT 为空（不需要中断）
  - 初始化本地门铃
  - 向 BSP 门铃发送就绪信号
  - 进入 AP 主循环（轮询自身门铃）

#### 4.4.3 看门狗 (`src/smp/watchdog.c` + `watchdog.h`)
- 数据结构:
  ```c
  typedef struct {
      volatile uint64_t heartbeat;    // 时间戳，由主核写入
      volatile uint64_t last_seen;    // 从核最后看到的心跳
      uint64_t          timeout_ms;   // 超时阈值 (默认 50ms)
  } watchdog_t;
  ```
- 心跳地址: 固定物理地址 `0xA000`
- BSP 每 1ms 写 TSC 值到 `heartbeat`
- AP 轮询 `heartbeat`，若 `TSC - heartbeat > timeout_cycles` 则触发复位
- 复位: 向 0xCF9 写 0x0E 执行硬复位
- 时间源: `rdtsc` 指令 (需在 boot.S 中校准 TSC 频率)

#### 4.4.4 自适应轮询退避 (`src/power/idle.c` + `idle.h`)
- 计数器: `idle_cycles` 记录连续空轮询次数
- 阈值: 2048 次
- 超过阈值: 执行 `MONITOR/MWAIT` 进入 C-state
- 门铃 `ring` 操作: 写操作自动唤醒 `MWAIT` 的核心
- 汇编:
  ```asm
  lea rax, [doorbell_addr]
  monitor
  xor ecx, ecx
  xor edx, edx
  mwait   ; 状态 0, 等待外部存储写
  ```

#### 4.4.5 主循环最终集成
- 整合所有子系统:
  ```c
  void main_loop(void) {
      uint64_t idle_count = 0;
      while (1) {
          bool did_work = false;
          
          // 键盘轮询
          if (keyboard_poll(&kbd)) { /* ... */ did_work = true; }
          // Shell 状态机
          if (shell_dispatch(&shell)) { did_work = true; }
          // 门铃轮询
          if (doorbell_poll(&my_doorbell)) { /* ... */ did_work = true; }
          // 看门狗心跳 (仅 BSP)
          if (is_bsp && tsc_since_last_heartbeat > MS_TO_CYCLES(1)) {
              write_heartbeat(rdtsc());
              did_work = true;
          }
          // 退避逻辑
          if (did_work) {
              idle_count = 0;
          } else {
              idle_count++;
              if (idle_count > 2048) {
                  monitor_mwait(&my_doorbell);
                  idle_count = 0;
              }
          }
      }
  }
  ```

#### 4.4.6 W4 验证
- 至少 2 核同时运行，各自输出信息到 VGA 不同区域
- 主核停止心跳 50ms 后，从核发起硬复位
- 长时间运行（24h）无崩溃
- 空转时功耗降低（MONITOR/MWAIT 生效）

## 5. 构建系统

### Makefile 目标
```makefile
CC       = ./x86_64-elf/bin/x86_64-elf-gcc
AS       = ./x86_64-elf/bin/as
LD       = ./x86_64-elf/bin/ld
OBJCOPY  = ./x86_64-elf/bin/objcopy

CFLAGS   = -m64 -mno-red-zone -nostdlib -ffreestanding \
           -fno-builtin -Wall -Wextra -O2 -mcmodel=kernel \
           -Isrc/include -Isrc/kernel -Isrc/mm -Isrc/ipc \
           -Isrc/task -Isrc/io -Isrc/smp -Isrc/power
ASFLAGS  = --64
LDFLAGS  = -T linker.ld -nostdlib

all: iso

kernel.bin: boot.o kmain.o ...
	$(LD) $(LDFLAGS) -o $@ $^

iso/kernel.bin: kernel.bin
	cp kernel.bin iso/boot/

bananaos.iso: iso/kernel.bin
	grub-mkrescue -o bananaos.iso iso/

run: bananaos.iso
	D:/qemu/qemu-system-x86_64 -cdrom bananaos.iso -nographic

clean:
	rm -f *.o *.bin *.iso
```

### 工具链说明
- 当前仅有 `as` 和 `ld` 在 `x86_64-elf/bin/` 目录下
- GCC 二进制约为 `x86_64-elf-gcc`（需确认是否存在）
- 若 GCC 不存在，需在 `x86_64-elf/bin/` 下补充

## 6. 假设与决策

| 项目 | 决策 | 理由 |
|------|------|------|
| 引导方式 | Multiboot (GRUB) | 简化开发，避免手动写引导扇区 |
| 最大核心数 | 4 (可扩展到 64) | 门铃页 4KB 可容纳 64 个 |
| NUMA 节点数 | 最多 4 个 | 与核心数对应 |
| 内存模型 | 平坦物理地址，mcmodel=kernel | 高 2GB 符号扩展，内核代码在 0xFFFFFFFF80000000 虚拟地址但物理地址在 1MB |
| 页表 | 2MB 巨型页 identity map | 最小化页表复杂度，虽然是分页模式但映射为 1:1 |
| 退避阈值 | 2048 次空轮询 | 经验值，平衡响应延迟与功耗 |
| 看门狗超时 | 50ms | 够短以快速恢复，够长以避免误触发 |
| 键盘协议 | PS/2 扫描码集 1 | 最广泛兼容 |
| GCC 依赖 | 假设 `x86_64-elf-gcc` 存在于工具链目录 | 需要确认，若无则需补充 |

> **注意关于分页**: 虽然 Axion 设计原则要求 "无分页" (CR0.PG=0)，但进入 x86-64 长模式需要启用分页。因此我们使用 identity mapping (1:1 映射) 来模拟无分页效果——所有虚拟地址等于物理地址。这是 x86-64 架构的硬性要求，无法绕过。

## 7. 验证步骤

### W1 验证
```powershell
cd d:\BananaOS-Axion
make run
# 预期: QEMU 窗口显示 "BananaOS v1.1"
```

### W2 验证
```powershell
make run
# 预期: 显示 NUMA 信息和内存分配成功消息
```

### W3 验证
```powershell
make run
# 预期: "bananaos>" 提示符，输入 help/ver/mem 有响应
```

### W4 验证
```powershell
make run
# 预期: 多核启动，看门狗工作正常，长时间运行稳定
```

## 8. 文件创建顺序

按依赖关系排列 (W1 → W4):

1. `src/include/bananaos.h` — 全局类型定义
2. `linker.ld` — 链接脚本
3. `src/boot/boot.S` — 引导汇编
4. `src/port.c` + `src/port.h` — I/O 端口
5. `src/vga.c` + `src/vga.h` — VGA 输出
6. `src/kernel/kmain.c` — 内核入口 (W1)
7. `Makefile` — 构建系统
8. `iso/grub/grub.cfg` — GRUB 配置
9. `src/mm/srat.c` + `srat.h` — SRAT 解析
10. `src/mm/pmalloc.c` + `pmalloc.h` — 内存分配器 (W2)
11. `src/ipc/doorbell.c` + `doorbell.h` — 门铃
12. `src/ipc/ringbuf.c` + `ringbuf.h` — 环形缓冲区
13. `src/task/statemachine.h` — 状态机框架
14. `src/io/keyboard.c` + `keyboard.h` — 键盘轮询 (W3)
15. `src/io/shell.c` + `shell.h` — Shell 状态机
16. `src/smp/ap.c` + `ap.h` — AP 启动 (W4)
17. `src/smp/watchdog.c` + `watchdog.h` — 看门狗
18. `src/power/idle.c` + `idle.h` — 退避