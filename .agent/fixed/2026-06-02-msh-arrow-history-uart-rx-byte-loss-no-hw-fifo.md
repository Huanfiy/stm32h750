# msh 上下键历史"概率性"翻不出 —— UART RX 满速丢字节（H7 硬件 FIFO 没开）

## 状态

**已修复（2026-06-02）。** 闭环定位:`msh` 历史功能本身正常,真因在**串口接收层满速丢字节**。修复为在 `drv_usart.c` 打开 H7 硬件 RX/TX FIFO。经新增 `test/cases/test_msh_history.py`(背靠背发上键 30 次,30/30 召回)+ 全套用例 7/7 验证。

## 现象

- 交互式按 ↑,要连按好几次才"概率性"翻出一次历史命令。
- 配置全对:`FINSH_USING_HISTORY` 开、`FINSH_HISTORY_LINES=5`、`FINSH_ECHO_DISABLE_DEFAULT` 未设;`shell.c` 方向键状态机(`WAIT_NORMAL→WAIT_SPEC_KEY→WAIT_FUNC_KEY`,识别 `0x1b 0x5b 0x41`)也正确。

## 定位

方向键是 3 字节转义序列 `1b 5b 41`,真实终端**零间隔背靠背**发出。PC 侧探针实测(`/dev/ttyUSB0` @115200):

- 一次 `write()` 发上键 `\x1b[A`,**20/20 完全无回显**;把 3 字节拉开间隔(≥2ms/字节)就能识别 → 纯接收时序问题。
- 满速一次性发 36 个不同字符,板子只回显 **15~18 个(丢约 50%)**,且**连续成串丢**(`c d e f g` 一起没、`j k l m` 一起没…)≈ 周期性 ~400µs 收不进来。
- 空回显的字节模式可对上状态机:`0x1b`→进 `WAIT_SPEC_KEY`(无回显)、`0x5b`→进 `WAIT_FUNC_KEY`(无回显),**突发最后一字节 `0x41` 丢失则啥也不发**(正好 `b''`);若丢的是 `0x5b`,`0x41` 会被当 'A' 回显——没出现,说明丢的是序列尾字节。

J-Link 实读 USART1(`0x40011000`)坐实硬件态:

| 位 | 修前 | 含义 |
|---|---|---|
| `CR1.RXNEIE` | 1 | 逐字节中断收 |
| `CR1.IDLEIE` | 0 | 无 IDLE 中断 → 不是 DMA |
| `CR1.FIFOEN`(bit29) | **0** | **硬件 FIFO 关闭** → RDR 仅 1+1 字节缓冲 |
| `CR3.DMAR` | 0 | RX 不走 DMA |

## 真因

uart1 控制台 = **逐字节中断 + 硬件 FIFO 关闭 + 无 DMA**。FIFO 关时每个字节必须在 1 个字节时间(115200 下 ≈87µs)内被 ISR 取走,否则 ORE 直接丢弃。本固件跑在 **QSPI XIP**,中断入口/处理都要从外部 flash 取指,延迟尖峰下逐字节硬截止扛不住 → 周期性连丢 → 三字节转义序列几乎不可能完整到达 → "概率性偶尔对齐才翻出一次"。

`stm32_putc` 是轮询 TC、**不关中断**,排除"echo 屏蔽 RX 中断";`finsh_getchar` 是 read-first 模式,排除"rx_sem 计数不匹配吞字节"。问题就在 HW 层缓冲不足 + ISR 延迟。

> `test/lib/serial_term.py` 开头注释早记录了这个现象(故发送限速到 25 char/s),但那只是测试侧绕过,交互式按方向键绕不了。

## 为什么修法不是 RX DMA(关键坑)

`shell.c:234` finsh **硬编码** `RT_DEVICE_FLAG_INT_RX | RT_DEVICE_FLAG_STREAM` 打开控制台,而 `dev_serial.c::rt_serial_open` 里 **`INT_RX` 分支优先于 `DMA_RX`**(`if INT_RX … else if DMA_RX`)。所以:

- 光在 menuconfig 勾 `BSP_UART1_RX_USING_DMA` —— 控制台**仍走逐字节中断,DMA 对 msh 不生效**;且 H7 `dma_config.h` 只配了 UART2 RX 块,要 DMA 还得手加 UART1 块。
- 想让控制台走 DMA 必须改 `~/SDK/rt-thread` 里的 finsh open flag —— **碰共享 SDK,违反 CLAUDE.md**。

→ 改用 **H7 硬件 RX FIFO**:在 `INT_RX` 模式下照样起作用(每字节仍触发 RXFNE 中断,但 FIFO 缓冲最多 16 字节),把 87µs **硬**截止变成 ~1.4ms **软**截止,实测 ~400µs 突发完全吃得下。纯 HAL 配置,不碰 finsh、不占 DMA stream。

## 修复

`libraries/HAL_Drivers/drivers/drv_usart.c::stm32_configure`,在 `HAL_UART_Init` 成功后(`SOC_SERIES_STM32H7` 守卫):

```c
HAL_UARTEx_SetRxFifoThreshold(&uart->handle, UART_RXFIFO_THRESHOLD_1_8);
HAL_UARTEx_SetTxFifoThreshold(&uart->handle, UART_TXFIFO_THRESHOLD_1_8);
HAL_UARTEx_EnableFifoMode(&uart->handle);
```

驱动的 RX drain 循环 `while(stm32_getc()!=-1)` 检测的是 `UART_FLAG_RXNE`(= H7 的 `RXNE_RXFNE`,FIFO 模式下即"FIFO 非空"),所以一次中断里能把 FIFO 里积压的多个字节一次抽干,无需改 drain 逻辑。阈值设 1/8 对 RXNEIE 路径其实不影响(RXFNE 一非空就触发),设上只为完整。

## 复验路径(已验证 PASS)

| 指标 | 修前 | 修后 |
|---|---|---|
| `CR1.FIFOEN` | 0 | **1** |
| 满速 36 字节背靠背回显 | ~50%(15~18/36) | **100%(180/180)** |
| 上键历史召回 | 0/20 | **30/30(确定性)** |
| `python3 test/run_all.py` | — | **7/7 PASS** |

`test/cases/test_msh_history.py`:seed 一条 `version` 进历史,背靠背发上键 30 次,要求 30/30 都把 `version` 召回(从"概率性"变"确定性"即为通过)。

## 通用教训

1. **多字节输入(方向键/粘贴)出问题先怀疑满速丢字节**,不是上层逻辑;用"满速发 N 个不同字符、数回显缺哪些"一招即可定性,丢字节模式还能反推状态机吞在哪一字节。
2. **QSPI XIP 固件 + 逐字节中断 + HW FIFO 关 = 满速必丢**。H7 自带 16 字节 UART FIFO,RT-Thread `drv_usart` 默认不开,XIP 工程务必开。
3. **finsh 控制台硬编码 `INT_RX` 且 `INT_RX` 优先于 `DMA_RX`** —— 在本 BSP 上想靠 `BSP_UARTx_RX_USING_DMA` 提速控制台是无效的,FIFO 才是不碰 SDK 的正解。其它显式以 `DMA_RX` 打开的 uart 消费者才吃得到 DMA。
