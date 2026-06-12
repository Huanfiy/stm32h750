# J-Link 通过自定义 Open Flashloader 烧录 W25Q64

## 背景

- 日期：2026-05-28
- 命令：`./run.sh flash-app`（外部 QSPI 烧录通道）
- 目标芯片：`STM32H750VB`
- 外部 Flash：`W25Q64JV`（8 MB / 64 Mbit，JEDEC `0xEF4017`）
- QSPI 引脚（CubeMX 配置）：
  - `PB2  AF9  QSPI_CLK`
  - `PB6  AF10 QSPI_BK1_NCS`
  - `PD11 AF9  QSPI_BK1_IO0`
  - `PD12 AF9  QSPI_BK1_IO1`
  - `PE2  AF9  QSPI_BK1_IO2`
  - `PD13 AF9  QSPI_BK1_IO3`
- 烧录工具：`JLinkExe V8.98`，J-Link V9.70，SWD 4 MHz

## 现象

### 现象 1：J-Link 内置 H7 QSPI loader 不识别本板

```text
J-Link>device STM32H750VB
J-Link>loadbin build/rt-thread.bin, 0x90000000
'loadbin': Performing implicit reset & halt of MCU.
Downloading file [build/rt-thread.bin]...
****** Error: SEGGER_OPEN_GetFlashInfo(): Algo reported a flash size of 0 bytes.
Error while determining flash info (Bank @ 0x90000000)
```

J-Link `ExpDevList` 显示 STM32H750VB 内置声明了 `0x90000000 / 0x10000000` 的 QSPI bank，但 SFDP 探测失败。

### 现象 2：自定义 device 名连不上

第一次尝试给设备命名 `STM32H750VB_W25Q64`：

```text
Device "STM32H750VB_W25Q64" selected.
Found SW-DP with ID 0x6BA02477
Failed to power up DAP
Error occurred: Could not connect to the target device.
```

补一个 `STM32H7_DAP_PowerUp.JLinkScript` 后改报：

```text
Error: Failed to configure AP.
Connecting to CPU via connect under reset failed.
```

### 现象 3：自写 loader 通过后 verify 失败

实现 `Init/UnInit/EraseSector/ProgramPage` 后：

```text
J-Link: Flash download: Bank 1 @ 0x90000000: 1 range affected (176128 bytes)
J-Link: Flash download: Total: 10.143s (Erase: 2.667s, Program: 7.253s)
****** Error: Verification failed @ address 0x90000000
```

## 结论

### 现象 1

J-Link 内置 H750 QSPI loader 假设 ST 官方评估板（H750B-DK / H7B3I-DK）的固定引脚映射（`PG6 NCS / PF10 CLK / PF6-9 IO`），与本板 `PB2/PB6/PD11-13/PE2` 不匹配。SFDP 探测发生在 loader 已经接管 QSPI 控制器之后，但 loader 把 QSPI 时钟挂到错的 GPIO，永远读不到 W25Q64 的 SFDP 响应，所以报 "flash size of 0"。

### 现象 2

J-Link DLL 中 STM32H7 系列的 `Power up DAP` / `Configure AP` 序列硬编码在 builtin device profile 内部，**不会**自动应用到通过 `JLinkDevices.xml` 注册的自定义 device 名。仅靠 JLinkScript hook 提前发 `CDBGPWRUPREQ` / `CSYSPWRUPREQ` 也不够 —— builtin 还在 `Configure AP` 时做 H7 专用的 D1/D3 power request、CSW 配置等动作。

### 现象 3

`SEGGER Open Flashloader` 协议下，J-Link 写完后会做一轮 verify。默认 verify 路径是 **直接通过 SWD 读 flash 区**（这里就是 `0x90000000`）。我们的 `Init()` 把 QSPI 配置成 **indirect** 模式（用于发 SPI 命令做 erase/program），CPU/SWD 此时无法直接访问 `0x90000000`，读回全部 `0xFF`，比对自然失败。

## 修复

### Step 1：手写 Open Flashloader

新增 `tools/flashloader/`：

- [`FlashOS.h`](../../tools/flashloader/FlashOS.h) — Keil/CMSIS 风格的 `FlashDevice` 结构 + 入口函数原型
- [`FlashPrg.c`](../../tools/flashloader/FlashPrg.c) — `Init/UnInit/EraseSector/EraseChip/ProgramPage/BlankCheck/SEGGER_OPEN_Read`，纯寄存器操作（无 HAL、无 libc），跑在 AXI-SRAM `0x24000000`
- [`FlashDev.c`](../../tools/flashloader/FlashDev.c) — `FlashDevice` 结构（base `0x90000000`，8 MB，4 KB sector，256 B page，JEDEC W25Q64JV）
- [`flashloader.lds`](../../tools/flashloader/flashloader.lds) — RAM 链接脚本；用 `EXTERN(Init UnInit ...)` 让 `--gc-sections` 不丢入口
- [`Makefile`](../../tools/flashloader/Makefile) — `make`/`make install` 生成 `STM32H750VB_W25Q64.FLM` 并复制到 `~/.config/SEGGER/JLinkDevices/Custom/STM32H750VB_W25Q64/`

关键点：

- 每个 J-Link 入口函数定义前要写 `__attribute__((used))`，写在 `FlashOS.h` 的声明上**不生效**
- 同时在 `flashloader.lds` 用 `EXTERN(Init UnInit EraseSector EraseChip ProgramPage BlankCheck SEGGER_OPEN_Read FlashDevice)` 把符号当根

### Step 2：用同名覆盖 builtin

修复现象 2。把 [`tools/flashloader/JLinkDevices.xml`](../../tools/flashloader/JLinkDevices.xml) 里的 `ChipInfo Name` 改成 builtin 同名 `STM32H750VB`：

```xml
<ChipInfo Vendor="ST" Name="STM32H750VB" Core="JLINK_CORE_CORTEX_M7"
          WorkRAMAddr="0x24000000" WorkRAMSize="0x00080000" />
<FlashBankInfo Name="QSPI W25Q64JV"
               BaseAddr="0x90000000" MaxSize="0x00800000"
               Loader="STM32H750VB_W25Q64.FLM"
               LoaderType="FLASH_ALGO_TYPE_OPEN" AlwaysPresent="1" />
```

`JLinkExe ExpDevList` 复验：

```text
"ST", "STM32H750VB", "Cortex-M7", { {0x08000000, 0x00020000}, {0x90000000, 0x00800000} }, {0x24000000, 0x00080000}
```

QSPI bank 已经从 builtin 的 `{0x90000000, 0x10000000}` 缩到 `{0x90000000, 0x00800000}` 并指向自定义 loader，**internal flash bank 仍由 builtin 接管，`boot-flash` 不受影响**。

### Step 3：实现 `SEGGER_OPEN_Read`

修复现象 3。

```c
__attribute__((used))
int SEGGER_OPEN_Read(U32 adr, U32 sz, U8 *buf)
{
    U32 off = adr - 0x90000000U;
    /* Fast Read (0x0B): 1-1-1, 8 dummy cycles, 24-bit address. */
    QSPI_DLR = sz - 1;
    QSPI_CCR = CCR_FMODE_RD | CCR_IMODE_1L | CCR_ADMODE_1L | CCR_ADSIZE_24
             | CCR_DCYC(8) | CCR_DMODE_1L | 0x0B;
    QSPI_AR  = off;
    for (U32 i = 0; i < sz; i++) {
        while (!(QSPI_SR & SR_FTF)) { ... }
        buf[i] = QSPI_DR_BYTE;
    }
    return (int)sz;
}
```

J-Link 一旦发现 `SEGGER_OPEN_Read` 符号存在，verify / compare 阶段会走 loader 自己的读路径而不是 SWD 直读。

## 验证

```bash
./run.sh app-rebuild-flash
```

成功输出：

```text
J-Link: Flash download: Bank 1 @ 0x90000000: 2 ranges affected (143360 bytes)
J-Link: Flash download: Total: 25.682s (Compare: 12.644s, Erase: 2.169s, Program: 6.021s, Verify: 4.723s)
J-Link: Flash download: Program speed: 22 KB/s
```

复位后串口端到端通：

```text
[BOOT] STM32H750 v0.1 ...
[BOOT] JEDEC ID = 0xEF4017
[BOOT] memory-mapped @ 0x90000000 ok
[BOOT] jump 0x90000000

 \ | /
- RT -     Thread Operating System
 / | \     5.2.0 build May 28 2026
msh />
```

## 后续边界

- `JLinkDevices.xml` 的 `Name="STM32H750VB"` 同名覆盖只影响**当前用户**（`~/.config/SEGGER/JLinkDevices/Custom/`），不污染系统级 J-Link 安装
- W25Q64JV 命令集是 1-1-1 写 + 1-1-1 读（0x0B Fast Read）。如果换 W25Q128 / W25Q256 等，page/sector size 一致可直接复用；如果换非 W25Q 系列（SFDP 自检差异大），需要重写 `Init` 中的 QE-bit 处理
- bootloader 里把 W25Q 切到 1-4-4（0xEB）只用于 XIP 读，**与本 loader 的 1-1-1 写无冲突**；loader Init 时会先发 reset 序列把芯片拉回 SPI 模式
