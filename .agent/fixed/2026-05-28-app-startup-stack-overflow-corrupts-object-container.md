# App 启动栈溢出污染 `_object_container` 导致 RT-Thread 卡死

## 背景

- 日期：2026-05-28
- 命令：`./run.sh reset`（在主固件迁到 QSPI XIP 之后第一次跑起来）
- 现象：bootloader 日志完整打印并 `jump 0x90000000`，但之后 RT-Thread 5.2.0 的 banner / `msh />` 提示从未出现，串口 8 s 内无任何后续输出

```text
[BOOT] STM32H750 v0.1 ...
[BOOT] memory-mapped @ 0x90000000 ok
[BOOT] sniff: SP=0x20000760 PC=0x9000579D
[BOOT] jump 0x90000000
===== end =====
```

## 结论

`board/linker_scripts/link.lds` 中 `_system_stack_size = 0x200`（512 B）**不足以**容纳 `rt_hw_board_init` 调用链 `SystemClock_Config → PeriphCommonClock_Config → HAL_RCCEx_PeriphCLKConfig → RCCEx_PLL2_Config → HAL_GetTick`（实测峰值用量约 700 B）。栈向下溢出穿入 `.data` 段尾部，**覆盖** `_object_container[]` 数组中 `RT_Object_Class_Memory` (`type = 0x0C`) 这一条目，把它从 `{12, list 自指, 40}` 写成全 0 + 一些 stack 残留。

随后启动序列调到 `rt_system_heap_init → rt_smem_init → rt_object_init(&m->parent, RT_Object_Class_Memory, name)`，`rt_object_get_information(Memory)` 在被破坏的 container 里找不到对应 entry 返回 `NULL`，触发：

```c
RT_ASSERT(information != RT_NULL);    /* object.c:359 */
```

`rt_assert_handler` 内部死循环（等 `[r7+23]` 的本地 `rc` 变非零，但永不变），CPU 停在 `0x90021BC6` 自旋。因为 console 尚未注册（assert 触发在 `rt_hw_usart_init` 之前），`rt_kprintf` 把 assert 信息 silently 丢弃，串口零输出。

## 关键证据

### J-Link halt 后状态

```text
PC = 0x90021BC8                     ← rt_assert_handler 内死循环
LR(saved) = 0x900217DF              ← rt_backtrace_frame
SP(MSP) = 0x200006A8
IPSR = 000 (NoException)
CFSR = 0x00000000
HFSR = 0x00000000
```

没有任何 fault —— 这是 RT_ASSERT，不是 HardFault。

### MSP 帧扫描出的调用链

```text
mem32 0x200006A8 32
200006B8 = ... 90022457 ...         ← caller of rt_assert_handler
200006E8 = ... 90021D47 ...         ← rt_smem_init
```

```bash
arm-none-eabi-addr2line -e build/rt-thread.elf -f -C 0x90022456 0x90021D46
# rt_object_init   object.c:359
# rt_smem_init     mem.c:201
```

### `rt_object_init` 入口反汇编

```text
90022438:  movs r3, #0
9002243A:  ldrb r3, [r7, #11]
9002243C:  mov r0, r3
9002243E:  bl 900223CC <rt_object_get_information>
90022442:  str r0, [r7, #24]
90022444:  ldr r3, [r7, #24]
90022446:  cmp r3, #0
90022448:  bne.n 0x90022456                ← 非 NULL 跳过 assert
9002244A:  movw r2, #359
9002244E:  ldr r1, [pc, #168]              ← __FUNCTION__
90022450:  ldr r0, [pc, #168]              ← __FILE__
90022452:  bl 0x90021B9C <rt_assert_handler>
```

确认 `assert(information != RT_NULL)` 是 line 359 的 RT_ASSERT。

### `.data` 段比对

ELF `.data` 段（`objcopy --only-section=.data ... data.bin`）offset `0x4E0` 起：

```text
0c 00 00 00  e4 04 00 20  e4 04 00 20  28 00 00 00     ← type=12 Memory, list 自指, size=0x28
```

RAM 同位置（J-Link `savebin 0x20000000 0x560`）：

```text
00 00 00 00  00 00 00 00  00 00 00 00  00 00 00 00     ← 被零化
```

更早一点（offset `0x4A0-0x4BC`，紧挨 Memory entry 之前）RAM 出现了**函数地址**：

```text
0x200004A4: 0x9000DC87
0x200004BC: 0x9000C337
```

`addr2line` 解析：

```text
0x9000DC86 → RCCEx_PLL2_Config       stm32h7xx_hal_rcc_ex.c:3776
0x9000C336 → HAL_RCCEx_PeriphCLKConfig stm32h7xx_hal_rcc_ex.c:1334
```

这正是 `SystemClock_Config` 调用链上的两个栈帧 LR —— **栈从 `0x20000760` 一路往下，长到了 `0x200004A8`**，用量 ≈ 696 B，远超 512 B 的 `_system_stack_size`。

## 修复

文件：[`board/linker_scripts/link.lds`](../../board/linker_scripts/link.lds)

```diff
-_system_stack_size = 0x200;
+_system_stack_size = 0x800;
```

注：

- RT-Thread 切到线程调度后每个线程有自己的栈，启动栈只在 `rt_hw_board_init` 期间用一次
- 取 `0x800`（2 KB）留出 3× 峰值的余量，应对未来 HAL 调用栈进一步加深
- 这把 RAM 占用从 0x200=512 B 加大到 0x800=2 KB；DTCM 128 KB 中不到 2 %，没有压力

## 验证

```bash
./run.sh rebuild && ./run.sh app-flash && ./run.sh reset
```

串口端到端：

```text
[BOOT] sniff: SP=0x20000D60 PC=0x9000579D
[BOOT] jump 0x90000000

 \ | /
- RT -     Thread Operating System
 / | \     5.2.0 build May 28 2026 15:53:30
 2006 - 2024 Copyright by RT-Thread team
msh />
```

注意 `SP=0x20000D60`（= `_estack`）从原 `0x20000760` 上移了 `0x600`，正是 stack 加大的结果，bootloader 的合法性嗅探仍通过（SP 高字节 `0x20`、PC 高字节 `0x90`、thumb 位 1）。

## 后续边界

- 这是**栈大小问题**，跟"应用搬到 QSPI"没必然关系 —— 原 `0x08000000` build 也用 `_system_stack_size = 0x200`。它当时没崩，大概率是 .data 末尾没贴着 `_object_container[Memory]` 这种关键结构，溢出几个字节恰好压在不影响启动的全局变量上
- 后续若再加 HAL 模块（如 FDCAN/USB 复杂 init）或者打开 RT-Thread 的 RT_ASSERT_HANG_RT_BACKTRACE 等更深嵌套，应再观察一次启动栈用量
- 推荐排查类似 "启动 silently 卡死、串口零输出" 时优先做：
  1. J-Link halt + `regs` 看 PC / IPSR
  2. `arm-none-eabi-addr2line` 解析 PC
  3. 若 PC 在 `rt_assert_handler` 内，扫 MSP 栈帧找真正 caller chain
  4. 若 caller 在 `rt_object_init`，对比 `.data` 段（ELF vs RAM）确认 `_object_container` 是否被踩
