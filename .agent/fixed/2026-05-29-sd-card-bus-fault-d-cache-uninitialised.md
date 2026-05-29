# 接入 SD 卡后开机 BusFault（D-cache 未初始化 + cache_buf 落在 DTCM）

## 背景

- 日期：2026-05-29
- 现象：插入 SD 卡按硬件复位后，启动到 msh 提示符后片刻 `mmcsd_de` 线程 hard fault，dump 显示 `SCB_CFSR_BFSR:0x04 IMPRECISERR`，PC 在 `SCB_CleanInvalidateDCache`（由 `rthw_sdio_send_command` 在 SD card capacity 打印后首次块读时调用），系统挂死。

```text
msh />[I/SDIO] SD card capacity 15558144 KB.
...
pc: 0x90004ba0   ← SCB_CleanInvalidateDCache 内 DCISW 循环
lr: 0x90004eff   ← rthw_sdio_send_command:270
hard fault on thread: mmcsd_de
bus fault: SCB_CFSR_BFSR:0x04 IMPRECISERR
```

## 结论

两个互相耦合的缺陷叠加：

1. **直接触发**：Cortex-M7 复位后 D-cache tag/data RAM 内容**未定义**。某些 set/way 可能被标记为 `valid+dirty` 但 tag 是垃圾物理地址。本 BSP 未定义 `BSP_SCB_ENABLE_D_CACHE`，从未做过显式 `SCB_InvalidateDCache()`。`drv_sdmmc.c::rthw_sdio_send_command` 在 IDMA 配置前调 `SCB_CleanInvalidateDCache()`（DCCISW 全 set/way clean+invalidate）→ 那些 dirty 假行被写回到垃圾地址 → 异步 BusFault `IMPRECISERR`（`BFAR` 不 valid）。
2. **潜在故障**：`drv_sdmmc.c::cache_buf` 是 `static rt_uint8_t cache_buf[4096]`，落在 `.bss` 即 DTCM (`RAM @ 0x20000000`)。STM32H7 SDMMC1 IDMA 是 D1 域 AHB master，**无法访问 DTCM**（DTCM 仅 CPU 直连）。即使绕过缺陷 1，首次块读 IDMA 写 DTCM 也会同样触发 imprecise BusFault。
3. **并发约束破坏**：`link.lds` 把 `.dma_buffer` 段预留在 AXI SRAM (`RAM_D1 @ 0x24000000`) 头部并导出 `_dma_buffer_end` 供 heap 偏移使用，但 `board/board.c::init_sram` 注册 `axi_sram` memheap 时从 `0x24000000` 起整段 512 KB，会把 heap 头写到 `.dma_buffer` 区域的前 24 字节，覆盖刚迁过去的 `cache_buf`（以及现有 `adc1_dma_buf`）。

## 关键证据

### 崩溃信息（修复前）

```
psr: 0x01000000          ← thread mode, T=1
r01: 0xe000ed00          ← SCB base
r02: 0x40000000          ← DCISW set 索引 (1 << 30)
pc:  0x90004ba0          ← SCB_CleanInvalidateDCache+0x48 (DCISW 循环内)
lr:  0x90004eff          ← rthw_sdio_send_command:270 之后
SCB_CFSR_BFSR: 0x04 IMPRECISERR
mmcsd_de  22  running 0x00000370 0x00000400  85%  EINTRPT/OK
```

`addr2line` 解析：
```
0x90004ba0 -> SCB_CleanInvalidateDCache (core_cm7.h:2449)
0x90004eff -> rthw_sdio_send_command (drv_sdmmc.c:270)  ; SCB_CleanInvalidateDCache() 调用点
```

### 调整 1（仅修 cache_buf 落点）后仍崩溃

`cache_buf` 从 DTCM 迁到 AXI SRAM，`nm` 确认地址为 `0x24000020`，对应 IDMA 可达区域。重测崩溃完全相同（同样 PC/LR/CFSR），说明触发点不在 IDMA 目的地，而在 `SCB_CleanInvalidateDCache` 自身遍历 set/way 时对未初始化垃圾 tag 行的写回。

### 调整 2（启动早期纯失效 D-cache）后 PASS

加 `SCB_InvalidateDCache()`（仅 DCISW invalidate，无 writeback）于 `rt_hw_board_init` 最早处后，闭环用例：
```
[I/SDIO] SD card capacity 15558144 KB.
msh /> version  ← msh 仍响应
PASS: SDIO brought up 15558144 KB card, msh remains responsive
```

## 修复

文件：`board/board.c`（启动早期清除未初始化 cache tag）

```c
void rt_hw_board_init(void)
{
    /* Cortex-M7 cache tag/data RAM is undefined out of reset. ... */
    SCB_InvalidateICache();
    SCB_InvalidateDCache();

    mpu_config_qspi_xip();
    ...
}
```

文件：`libraries/HAL_Drivers/drivers/drv_sdmmc.c`（IDMA 目的缓冲迁出 DTCM）

```c
rt_align(SDIO_ALIGN_LEN)
static rt_uint8_t cache_buf[SDIO_BUFF_SIZE]
    __attribute__((section(".dma_buffer")));   /* AXI SRAM, IDMA-reachable */
```

文件：`board/board.c`（让 axi_sram / sram4 heap 跳过 `.dma_buffer` / `.ram_d3` 预留段）

```c
extern char _dma_buffer_end[];
extern char _ram_d3_end[];
...
rt_memheap_init(&_heap_axi_sram, "axi_sram", (void *)_dma_buffer_end,
                AXI_SRAM_END - (rt_uint32_t)_dma_buffer_end);
rt_memheap_init(&_heap_sram4, "sram4", (void *)_ram_d3_end,
                SRAM4_END - (rt_uint32_t)_ram_d3_end);
```

## 验证

新增 `test/cases/test_sd.py`（闭环用例，挂 `CASE_ORDER`）：复位 → 期待 msh → 拒绝出现 `hard fault` → 期待 `SD card capacity \d+ KB` → 期待 `version` 命令仍能拿到 RT-Thread banner。
- 卡未插：SKIP（autotools 77，符合 harness 约定）。
- 卡已插：PASS，无 hard fault，`SD card capacity 15558144 KB`，msh 持续响应。
- 全套 `test/run_all.py`：`test_swd / test_boot / test_adc / test_can` 仍 PASS。

## 教训

- Cortex-M7 上，**不要在没有先 invalidate 的前提下调任何 DCCISW（clean+invalidate by set/way）**。复位后 cache RAM 内容未定义，遍历全 set/way 写回到垃圾地址即触发异步 BusFault。`SCB_EnableDCache()` 内部已含这一步；若 BSP 不开 D-cache，仍需在启动早期手动 `SCB_InvalidateDCache()`。
- STM32H7 上凡是 DMA 目的/源缓冲一律不能落在 DTCM。`link.lds` 已经给出 `.dma_buffer` / `.ram_d3` 约定（AXI SRAM / SRAM4），所有驱动迁过来即可。memheap 注册必须用 `_dma_buffer_end` / `_ram_d3_end` 偏移过这两段预留，否则 heap 头会覆盖 DMA 缓冲。
