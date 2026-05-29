# STM32H7 SDMMC ACMD51 IDMA hang —— SD 卡识别到容量后停在 SCR 读取

## 状态

未修复。`drv_sdmmc.c` 的 IDMA 数据通路对 < 16 B（4 word）的小尺寸传输不稳定，导致 SD 卡识别在 ACMD51（SEND_SCR，8 字节）这一步挂死，`sd0` 块设备无法注册，`dfs_mount` / `auto-mount` 自然失败。本档把诊断证据与可能的修复路径写下来，避免下次重复挖。

## 背景

- 日期：2026-05-29
- 触发场景：开启 `RT_USING_DFS_ELMFAT` + 启用 `BSP_USING_SDIO`/`BSP_USING_SDIO1` 并尝试 `dfs_mount("sd0", "/", "elm", 0, 0)`。
- 现象：
  ```
  [I/SDIO] SD card capacity 15558144 KB.
  [E/drv.sdmmc] wait cmd completed timeout
  [FS] sd0 not present after 10000ms — card missing?
  ```
- 系统稳定，无 hard fault，msh 仍响应；`list device` 不含 `sd0`，`list_blk` 表头为空。

## 诊断证据

在 `rthw_sdio_wait_completed` 超时分支临时打印 cmd / STA：

```
wait timeout cmd=51 arg=0x00000000 STA=0x00101000 CLKCR=0x00000050 DCOUNT=0
```

解读：
- `cmd=51` → ACMD51（SEND_SCR）。RT-Thread `mmcsd_core` 在 CSD 解析后立即发，用于探卡的总线宽度能力。
- `arg=0` → ACMD51 固定参数。
- `STA=0x00101000` → bit 12 `DPSMACT`（数据路径 SM 仍激活）+ bit 20 `BUSYD0`（D0 被卡拉低）。`CMDREND` (bit 6) 没设，`CMDSENT` (bit 7) 没设，`DATAEND` (bit 8) 没设。
- `CLKCR=0x00000050` → CLKDIV=80，WIDBUS=00（1-bit）。与 `SDIO_USING_1_BIT` 试探结果一致（即便切到 1-bit 也仍 hang，所以排除 bus-width 时序）。
- `DCOUNT=0` → SDMMC 端认为字节计数已用完。

总结：CPSM 早已发出命令、IDMA 也搬完（或没启动），但 DPSM 状态机不进 DATAEND，整个数据阶段卡死，超时 5 s 后软件路径放弃。

## 根因推测（待复核）

STM32H750 errata ES0392 关于 SDMMC IDMA 的几条相关项叠加：
1. SDMMC IDMA 期待"端口侧字节数为 4-word（16 B）的整数倍"才能正确发出 `DATAEND`。8 字节的 ACMD51 SCR 不满足。
2. `mmcsd_core` 对 SCR 用 `READ_SINGLE_BLOCK` 风格的数据传输路径走 IDMA，没有 fallback 到 FIFO 轮询，对短传输天然踩坑。
3. ST 自家 `HAL_SD_GetCardSCR()` 用 FIFO 轮询而非 IDMA，正是为了规避这一点。RT-Thread 的 `drv_sdmmc.c` 没沿用，所有数据传输都走 `IDMABASE0/IDMACTRL`。

## 已经尝试且无效

- `SDIO_USING_1_BIT`（强制 1-bit 总线）：CLKCR.WIDBUS 已验证从 01 切回 00，但 ACMD51 同样 hang。证明跟 bus-width 切换时机无关。
- 升 `mmcsd_de` 栈：与本 issue 无关（栈用量仅 71% 左右）。
- 启动早期 `SCB_InvalidateDCache()`、`cache_buf` 迁 AXI SRAM：那是另一个已修复的 issue（见 `2026-05-29-sd-card-bus-fault-d-cache-uninitialised.md`），与 ACMD51 hang 无关。

## 可能的修复方向（任选其一）

1. **驱动改造（推荐）**：在 `rthw_sdio_send_command` 给 `data->blks * data->blksize <= 16` 走 FIFO 轮询路径而非 IDMA：
   - 不写 `IDMACTRL = IDMAEN`
   - CMD 发出后轮询 `STA.RXFIFOHF` / `RXFIFOF`，用 `LDR Rx, [hsd, #0x80]` 从 `SDMMC_FIFO` 读 word
   - 直到 `STA.DATAEND` 置位为止
   - 仅在 SCR / 类似小传输生效，主路径仍走 IDMA 享受性能
2. **绕开 ACMD51**：patch `mmcsd_core/sd.c::mmcsd_get_scr`，在 `host->flags` 加一个标记位让它跳过 SCR 探测，强制按 4-bit 默认 capability 继续。代价是放弃精确的 SD 总线宽度协商。
3. **临时换驱动**：把 H7 改为走 `drv_sdio.c`（F4 时代驱动）。需要把 H7 的 SDMMC1 寄存器布局与 F4 SDIO 兼容，估计要写桥接，工作量与方案 1 接近。

## 现状基建（已落地、为修复做铺垫）

`fix(fs)` commit 中已提交：
- `board/Kconfig`：`BSP_USING_SDIO` 改为 `menuconfig`，嵌套 `BSP_USING_SDIO1` / `BSP_USING_SDIO2`。menuconfig 重生成后不再丢 `BSP_USING_SDIO1` 触发 `drv_sdmmc.c` 的 `#error`。
- `libraries/HAL_Drivers/drivers/drv_sdmmc.c`：`#include "drv_config.h"`，让 `h7/sdio_config.h` 中以后可能加的宏（如 `SDIO_USING_1_BIT`）真正能被驱动看到。
- `applications/app_drv/app_drv_fs.c`：`INIT_APP_EXPORT` 钩子轮询 `sd0` 出现后 `dfs_mount("sd0", "/", "elm", 0, 0)`。放在 init/main 线程上下文跑（栈 2 KB），避免 `tidle0` (256 B) 在 defunct 清理路径上被 DFS V2/FatFs 撑爆（实测换写法前 `[E/kernel.sched] thread:tidle0 stack overflow`）。
- `test/cases/test_fs.py`：spec 用例，断言 `ls /` 不出 `No such directory`。当前 PARKED，不在 `CASE_ORDER` 中。驱动修复后把 `test_fs.py` 加回 `CASE_ORDER`，预期 PASS。

## 复验路径

驱动修复落地后：
1. 把 `test_fs.py` 加回 `test/run_all.py::CASE_ORDER`。
2. 拔插 SD 卡触发探测，跑 `python3 test/cases/test_fs.py`。
3. 期望串口出现 `[FS] sd0 mounted at / as elm` 且 `ls /` 返回目录内容（含 FAT 卷标 / 文件名）。
