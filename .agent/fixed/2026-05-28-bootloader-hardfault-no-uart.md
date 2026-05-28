# Bootloader HardFault 导致串口无输出

## 背景

- 日期：2026-05-28
- 命令：`./run.sh boot-rebuild-flash`
- 现象：烧录后 `/dev/ttyUSB0` 无任何启动输出
- 目标芯片：`STM32H750VB`
- 调试接口：J-Link SWD
- 串口参数：`USART1 PB14/PB15, 115200 8N1`

## 结论

直接原因不是串口配置错误，而是 bootloader 在进入业务初始化前触发 HardFault。

根因位于 `bootloader/linker_scripts/boot.lds`：linker script 未保留完整的 `.init/.fini` 输入段，也未提供完整的 `preinit/init/fini_array` 边界。`Reset_Handler` 调用 `__libc_init_array()` 后，`_init` 只有 `crti.o` 的函数序言，没有 `crtn.o` 的返回尾部，执行流掉入后续数据区域，最终触发 undefined instruction HardFault。

## 关键证据

J-Link halt 后状态：

```text
PC = 08003328
XPSR IPSR = 003 (HardFault)
LR = FFFFFFF9
CFSR = 00010000
HFSR = 40000000
```

异常栈帧：

```text
stacked LR = 0800360D
stacked PC = 080041F4
stacked xPSR = 41000000
```

地址解析：

```bash
arm-none-eabi-addr2line -e bootloader/build/bootloader.elf -f -C 0x0800360c 0x080041f4
```

结果显示 `0x0800360c` 位于 `__libc_init_array`，`0x080041f4` 已落入非代码区域。

## 修复

文件：[bootloader/linker_scripts/boot.lds](../../bootloader/linker_scripts/boot.lds)

修复内容：

- 保留 `.init` 和 `.fini`
- 提供 `__preinit_array_start/end`
- 提供 `__init_array_start/end`
- 提供 `__fini_array_start/end`
- 保留 `.preinit_array*`、`.init_array*`、`.fini_array*`

修复后 `_init` 具备完整返回序列：

```text
080041c4 <_init>:
  push ...
  nop
  pop ...
  bx lr
```

## 验证

重编译并烧录：

```bash
./run.sh boot-rebuild
./run.sh boot-flash
```

串口验证：

```bash
stty -F /dev/ttyUSB0 115200 cs8 -cstopb -parenb raw -echo
timeout 5 cat /dev/ttyUSB0
```

实际输出：

```text
[BOOT] STM32H750 v0.1 -- May 28 2026 12:03:42
[BOOT] QSPI peripheral ready (FlashSize=22, 8MB)
[BOOT] JEDEC ID = 0xEF4017
[BOOT] QE bit ensured
[BOOT] memory-mapped @ 0x90000000 ok
[BOOT] sniff: SP=0xFFFFFFFF PC=0xFFFFFFFF
[BOOT] WARN: app header looks invalid; not jumping
[BOOT] idle
```

J-Link 复查：

```text
IPSR = 000 (NoException)
CFSR = 00000000
PC 位于 HAL_Delay idle loop
```

## 后续边界

当前 bootloader 已正常运行。仍停留在 idle 的原因是 QSPI `0x90000000` 处未发现有效应用向量表：

```text
SP=0xFFFFFFFF PC=0xFFFFFFFF
```

`boot-rebuild-flash` 只烧录 bootloader 到内部 Flash `0x08000000`，不负责将主固件写入 QSPI。应用启动验证需单独完成主固件构建、链接地址确认和 QSPI 写入流程。
