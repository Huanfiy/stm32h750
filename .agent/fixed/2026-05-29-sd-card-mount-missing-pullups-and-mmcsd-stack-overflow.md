# STM32H7 SD 卡上不来 —— 两个真根因（数据线缺上拉 + mmcsd 检测线程栈溢出）

## 状态

**已修复（2026-06-01）。** 闭环定位到**两个相互独立**的根因，均经 `test/cases/test_fs.py`（write→reset→readback）与全套用例 6/6 验证。

⚠️ 本档原标题为 "ACMD51 IDMA hang"，并把矛头指向"H7 SDMMC IDMA 对 < 16 B 短传输不发 DATAEND 的 errata"——**那是误判**。真因是硬件上 SD 数据线缺内部上拉；IDMA 对 8 B / 64 B 短读毫无问题（见下文"IDMA-only 实验"）。保留原始诊断是为了记下"同一个 STA=0x00101000 症状如何被误读成 IDMA 问题"，避免下次再走弯路。

## 根因 1 —— SD 数据线缺内部上拉（ACMD51 hang 的真因）

- 现象：`SD card capacity` 打印后停在 ACMD51（SEND_SCR，8 B），`rthw_sdio_wait_completed` 5 s 超时，`STA=0x00101000`：
  - bit 12 `DPSMACT`（数据路径状态机仍激活）
  - bit 20 `BUSYD0`（D0 被拉低 / 浮空）
  - `CMDREND`/`DATAEND` 均未置位
- 真因：`HAL_SD_MspInit`（`board/CubeMX_Config/Core/Src/stm32h7xx_hal_msp.c`）把 D0–D3(PC8–11) + CMD(PD2) 全配成 `GPIO_NOPULL`，板上又没有外部上拉。CMD **命令相**靠总线空闲电平勉强能过（所以能读到 CID/CSD、识别容量），但 **数据相** D0 一旦浮空，第一笔数据传输（SCR）就停在 `DPSMACT|BUSYD0`——SDMMC 在等 D0 的 start bit，永远等不到。
- 修复：D0–D3(PC8–11) + CMD(PD2) 改 `GPIO_PULLUP`，CK(PC12) 保持 `GPIO_NOPULL`（时钟由 host 驱动，不需要上拉）。对齐参考工程 `tool/stm32/h7_current_sample/Core/Src/stm32h7xx_hal_msp.c`。
- ✅ 修复已写回 `.ioc`：PC8/9/10/11 + PD2 加了 `GPIOParameters=GPIO_PuPd` / `GPIO_PuPd=GPIO_PULLUP`，PC12 不加（保持 NOPULL 默认）。所以 **CubeMX 重新生成会保留上拉**，不再打回 NOPULL——`stm32h7xx_hal_msp.c` 里那段 PULLUP 现在就是 CubeMX 生成出来的，无需事后手改。

## 根因 2 —— mmcsd 检测线程栈溢出踩坏 ADC DMA 句柄（枚举成功后整机假死）

上拉修好、`sd0` 能枚举之后，又撞上第二个独立 bug：

- 现象：`found part[0]` 之后 msh 彻底无响应（连回显都没）。J-Link halt：**非 hard fault**（CFSR/HFSR = 0），`XPSR` 的 IPSR = 27 = `DMA1_Stream0_IRQn`，PC 死循环在 `HAL_DMA_IRQHandler`，`DMA1_LISR` 的 TCIF/HTIF 清不掉。
- 定位链：dump `hdma_adc1`(0x200022E0) → `Instance` 不再是寄存器基址 `0x40020010`，而是被写成 RAM 指针 + QSPI 代码地址（句柄被踩烂）；`addr2line` 那些值落在 `devtmpfs_create_vnode` 深链；读 `mmcsd_de` 线程 TCB → 栈底 `0x20002918`、4 KB，峰值打到 `0x200022E0`（栈底下方 0x638，**峰值用栈约 5.7 KB**）。
- 真因：`mmcsd_detect` 线程把 `sd0` 注册进 **DFS V2 devfs**（`devtmpfs_create_vnode` 深递归）时峰值用栈 ~5.7 KB，**向下越过 4 KB 栈底，踩烂相邻 `.bss` 的 `hdma_adc1`/`hdma_adc3`**。句柄 `Instance` 被改写后，`DMA1_Stream0_IRQHandler → HAL_DMA_IRQHandler(&hdma_adc1)` 清的是错误地址的标志位 → 中断标志永远清不掉 → DMA1_Stream0 中断 storm → 所有线程被饿死。
- `RT_USING_OVERFLOW_CHECK` **查不到**这种"瞬时深递归后又退回"的溢出：线程被挂起时 sp 已经回到健康区，栈顶 magic 也没被持续覆盖。
- 修复：`RT_MMCSD_STACK_SIZE` 4096 → 8192（`rtconfig.h` + `.config` 同步，避免 menuconfig 重生成回退）。

## IDMA-only 实验（证伪 "IDMA 短传输 errata"）

定位根因 1 期间，曾在 `drv_sdmmc.c` 加了一条 `rthw_sdio_xfer_short_read()` FIFO 轮询绕路：对 `len <= 64 B` 的读（ACMD51 8 B、ACMD13 64 B、CMD6 64 B）不走 IDMA，改纯 CPU 轮询 FIFO。当时它"能让卡上来"，于是误以为坐实了 IDMA errata。

上拉修好后做对照实验：把 `SDIO_FIFO_POLL_MAX` 置 0，让**所有**读都强制走 IDMA，重新烧录跑 `test_sd` / `test_fs`：

```
[SDDBG] idma cmd=51 len=8  r cerr=0 derr=0 sta=0x00000000
[SDDBG] idma cmd=13 len=64 r cerr=0 derr=0 sta=0x00000000
[SDDBG] idma cmd=6  len=64 r cerr=0 derr=0 sta=0x00000000
...
[FS] sd0 mounted at / as elm     → 全过
```

结论：**IDMA 对 8 B / 64 B 短读完全正常**。FIFO 绕路当初之所以"有效"，只是因为它纯 CPU 读 FIFO、走的数据路径与 IDMA 不同，恰好避开了那次 hang；真正的病灶始终是缺上拉。实验通过后，`rthw_sdio_xfer_short_read()`、`SDIO_FIFO_POLL_MAX`、路由分支、以及两处 `[SDDBG]` 调试打印全部删除，驱动恢复纯 IDMA 单一路径（TEXT 减约 900 B）。

## 通用教训

H7 上"DMA1_Stream0 storm / 卡死在 `HAL_DMA_IRQHandler` 且 TCIF 清不掉"——**先怀疑 DMA 句柄结构体（在 `.bss`）被栈溢出/野指针踩坏，而不是 DMA 外设本身**。最快判据：dump 句柄看 `Instance` 是否还是外设寄存器基址。

SD 卡"识别得到容量、却停在第一笔数据传输（SCR）"且 `STA` 带 `BUSYD0`——**先查数据线上拉**，而不是去翻 IDMA errata。命令相能过不代表数据相能过。

## 已落地的基建（修复前铺垫，仍有效）

- `board/Kconfig`：`BSP_USING_SDIO` 改 `menuconfig`，嵌套 `BSP_USING_SDIO1` / `BSP_USING_SDIO2`，重生成不再丢 `BSP_USING_SDIO1` 触发 `#error`。
- `drv_sdmmc.c`：`#include "drv_config.h"`，暴露 `h7/sdio_config.h` 的宏（如 `SDIO_USING_1_BIT`）。
- `applications/app_drv/app_drv_fs.c`：`INIT_APP_EXPORT` 钩子轮询 `sd0` 出现后 `dfs_mount("sd0","/","elm",0,0)`；跑在 init/main 线程（栈 2 KB），避免 `tidle0`(256 B) 在 defunct 清理路径被 DFS V2/FatFs 撑爆。
- `cache_buf` 落在 `.dma_buffer`（AXI SRAM）：SDMMC1 IDMA（D1 AHB master）够不到 DTCM，放 DTCM 会在首次块读触发 imprecise BusFault（另见 `2026-05-29-sd-card-bus-fault-d-cache-uninitialised.md`）。

## 复验路径（已验证 PASS）

1. `./run.sh app-rebuild-flash`
2. `python3 test/cases/test_fs.py` —— 期望：`[FS] sd0 mounted at / as elm`，写入 nonce → reset（丢掉 DFS V2 pcache）→ 重新挂载后 `cat` 读回同一 nonce（证明字节真落到物理卡、且按 `begin: 4194304` 偏移正确寻址）。
3. `python3 test/run_all.py` —— 6/6 PASS（`test_adc` 16 通道在界内，证明被踩的 ADC DMA 句柄已恢复）。

相关：`test/run_all.py::CASE_ORDER` 已含 `test_fs.py`。
