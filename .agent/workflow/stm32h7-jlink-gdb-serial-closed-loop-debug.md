# STM32H7 Bootloader 闭环调试流程

## 适用场景

适用于 STM32H7 bootloader 或 RT-Thread 固件烧录后无串口输出、启动异常、跳转应用失败等问题。流程覆盖代码分析、构建烧录、J-Link halt、GDB 定位、串口复验。不覆盖硬件焊接、电源完整性、外设物理损坏等板级问题。

## 前置条件

- J-Link 可连接目标板，SWD 电压可读。
- 工具链路径可用：`$HOME/toolchain/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/bin`
- J-Link 工具可用：`JLinkExe`、`JLinkGDBServerCLExe`
- 串口设备存在，例如 `/dev/ttyUSB0`
- 串口参数明确，例如 `115200 8N1`

## 1. 先确认烧录对象

检查脚本入口，确认当前命令实际烧录的是 bootloader 还是主固件：

```bash
sed -n '1,240p' run.sh
```

重点检查：

- `boot-rebuild-flash` 是否只执行 `boot-clean; boot-build; boot-flash`
- bootloader 烧录地址是否为 `0x08000000`
- 主固件是否另有 `build/rtthread.bin`
- QSPI/XIP 应用是否需要单独写入 `0x90000000`

## 2. 代码路径审查

快速定位入口、UART 初始化、日志输出、异常处理：

```bash
sed -n '1,240p' bootloader/src/main.c
sed -n '1,260p' bootloader/src/boot_uart.c
sed -n '1,220p' bootloader/inc/boot_config.h
sed -n '1,220p' bootloader/linker_scripts/boot.lds
```

重点检查：

- 首条日志是否在 `boot_uart_init()` 后立即输出
- `BOOT_UART_INSTANCE`、波特率、TX/RX 引脚是否与硬件一致
- linker script 是否保留 `.isr_vector`、`.init/.fini`、`init_array`
- app 跳转地址和向量表检查条件是否正确

## 3. 构建并检查 ELF

构建：

```bash
./run.sh boot-rebuild
```

检查段布局：

```bash
arm-none-eabi-objdump -h bootloader/build_boot/bootloader.elf
```

检查关键符号：

```bash
arm-none-eabi-nm -n bootloader/build_boot/bootloader.elf \
  | rg 'Reset_Handler| main$|entry$|boot_uart_init|SystemClock_Config|Error_Handler|_init$|_fini$|__init_array'
```

检查向量表：

```bash
arm-none-eabi-objdump -s -j .text \
  --start-address=0x08000000 \
  --stop-address=0x08000100 \
  bootloader/build_boot/bootloader.elf
```

可接受现象：

- 初始 SP 指向 SRAM，例如 `0x2000xxxx`
- Reset_Handler 指向内部 Flash，例如 `0x0800xxxx | 1`
- `Reset_Handler -> entry -> main` 路径可解析

## 4. 烧录并抓串口

烧录：

```bash
./run.sh boot-flash
```

串口监听：

```bash
stty -F /dev/ttyUSB0 115200 cs8 -cstopb -parenb raw -echo
timeout 5 cat /dev/ttyUSB0
```

复位后抓完整启动日志：

```bash
(stty -F /dev/ttyUSB0 115200 cs8 -cstopb -parenb raw -echo; timeout 5 cat /dev/ttyUSB0 > /tmp/boot-serial.log) &
reader=$!
sleep 0.5
./run.sh reset
wait $reader || true
sed -n '1,120p' /tmp/boot-serial.log
```

如果仍无输出，进入 J-Link/GDB 定位。

## 5. J-Link 快速 halt

读取寄存器、Flash 向量表和 fault 状态：

```bash
tmp=$(mktemp)
printf '%s\n' \
  'si SWD' \
  'speed 4000' \
  'device STM32H750VB' \
  'connect' \
  'h' \
  'regs' \
  'mem32 0x08000000,8' \
  'mem32 0xE000ED28,1' \
  'mem32 0xE000ED2C,1' \
  'mem32 0xE000ED34,1' \
  'mem32 0xE000ED38,1' \
  'mem32 0xE000ED3C,1' \
  'exit' > "$tmp"
JLinkExe -AutoConnect 1 -ExitOnError 1 -CommandFile "$tmp"
rm -f "$tmp"
```

寄存器含义：

- `IPSR = 003` 表示 HardFault
- `CFSR = 0x00010000` 表示 UsageFault.UNDEFINSTR
- `HFSR = 0x40000000` 表示 forced HardFault
- `PC` 若位于默认异常死循环，需读取异常栈帧找真实触发地址

## 6. 读取异常栈帧

根据 `LR=FFFFFFF9` 可判断使用 MSP。读取 MSP 附近 8 个 word：

```bash
tmp=$(mktemp)
printf '%s\n' \
  'si SWD' \
  'speed 4000' \
  'device STM32H750VB' \
  'connect' \
  'h' \
  'regs' \
  'mem32 0x20000800,8' \
  'exit' > "$tmp"
JLinkExe -AutoConnect 1 -ExitOnError 1 -CommandFile "$tmp"
rm -f "$tmp"
```

异常栈帧布局：

```text
SP+00 R0
SP+04 R1
SP+08 R2
SP+0C R3
SP+10 R12
SP+14 LR
SP+18 PC
SP+1C xPSR
```

定位真实触发地址：

```bash
arm-none-eabi-addr2line -e bootloader/build_boot/bootloader.elf -f -C <stacked_lr> <stacked_pc>
arm-none-eabi-objdump -d --start-address=<addr_before> --stop-address=<addr_after> bootloader/build_boot/bootloader.elf
```

## 7. 使用 GDB 交互调试

启动 GDB Server：

```bash
JLinkGDBServerCLExe -device STM32H750VB -if SWD -speed 4000 -port 2331 -nogui
```

另一个终端进入 GDB：

```bash
arm-none-eabi-gdb bootloader/build_boot/bootloader.elf
```

GDB 命令：

```gdb
target remote :2331
monitor reset halt
info registers
x/8wx 0x08000000
b Reset_Handler
b main
b boot_uart_init
continue
```

HardFault 后读取 fault 寄存器：

```gdb
x/wx 0xE000ED28
x/wx 0xE000ED2C
x/wx 0xE000ED34
x/wx 0xE000ED38
x/wx 0xE000ED3C
```

读取 MSP 栈帧：

```gdb
p/x $msp
x/8wx $msp
```

## 8. 修复后复验标准

复验必须同时覆盖三类证据：

1. 构建证据：`./run.sh boot-rebuild` 成功，ELF 段布局符合预期。
2. 调试证据：J-Link halt 后 `IPSR = 000 (NoException)`，`CFSR = 00000000`。
3. 串口证据：复位后可看到首条 boot 日志和后续状态日志。

示例串口输出：

```text
[BOOT] STM32H750 v0.1 -- <build date> <build time>
[BOOT] QSPI peripheral ready (FlashSize=22, 8MB)
[BOOT] JEDEC ID = 0xEF4017
[BOOT] QE bit ensured
[BOOT] memory-mapped @ 0x90000000 ok
```

如果输出中出现：

```text
[BOOT] sniff: SP=0xFFFFFFFF PC=0xFFFFFFFF
[BOOT] WARN: app header looks invalid; not jumping
```

表示 bootloader 已运行，后续问题转为 QSPI 应用镜像缺失或应用向量表无效，不再归类为 bootloader 串口无输出问题。
