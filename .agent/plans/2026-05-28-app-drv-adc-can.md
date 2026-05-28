# app_drv_adc / app_drv_can 应用层驱动落地计划

## Context

RT-Thread 的 `rt_adc_device` 框架是轮询式的，每次只能读一路、不支持 DMA/扫描，满足不了 16 路同步采样的需求；同时 `libraries/HAL_Drivers/drivers/drv_can.c` 只覆盖 F1/F4/F7/L4 的 bxCAN，**没有任何 H7-FDCAN 分支**，盲目 `select RT_USING_CAN` 会直接编译失败。两路都得绕过 RT-Thread 设备层，改为应用层驱动。

本计划把 16 路电流采样设计从 `tool/stm32/h7_current_sample/Core/Src/main.c` 迁移过来（设计完全成熟、已经在另一块板上跑通），并在同一块板上新增一个 FDCAN2 数据链路层（500 kbps Classic CAN，单例句柄、对外提供 `send/recv` 阻塞接口）。两路驱动都按"信号量 + 快照/FIFO 缓存"的 RT-Thread 风格暴露给业务层。

## 输出物概览

```
applications/
├── main.c                    # 改：在 main 里 app_drv_adc_init() + app_drv_can_init()
└── app_drv/
    ├── SConscript            # 新：Glob('*.c')
    ├── app_drv_adc.h         # 新
    ├── app_drv_adc.c         # 新
    ├── app_drv_can.h         # 新
    └── app_drv_can.c         # 新
board/
└── linker_scripts/link.lds   # 改：新增 RAM_D1 / RAM_D3 + .dma_buffer / .ram_d3 段
```

`board/CubeMX_Config/Core/Src/main.c` 里的 `MX_ADC1_Init / MX_ADC3_Init / MX_TIM6_Init / MX_FDCAN2_Init / MX_DMA_Init / MX_BDMA_Init` **不接入编译**，由 `app_drv_*.c` 自己重新实现等价初始化（CubeMX 用过的参数表照搬，作为权威来源）。MSP 已在 `board/CubeMX_Config/Core/Src/stm32h7xx_hal_msp.c` 中编入（GPIO、外设时钟、HAL_FDCAN_MspInit 都已就位），无需重复劳动。

## 关键约束 / 踩坑预防

1. **DMA 缓冲所在域**：
   - DMA1（D2 域）**不能访问 DTCM**（0x20000000），ADC1 DMA buffer 必须落到 AXI-SRAM (`0x24000000`) 或 SRAM1/2/3。
   - BDMA（D3 域）**只能访问 SRAM4** (`0x38000000`) 和 BKPSRAM；ADC3 DMA buffer 必须落到 SRAM4。
   - `board/board.c:104-119` 的 `init_sram` 在 `INIT_BOARD_EXPORT` 阶段把 AXI/SRAM1-4/Backup 注册成 memheap，**会接管这些区域**——所以 DMA buffer 不能走 `rt_malloc` 借 memheap（容易碎片化、且 memheap 起点和 buffer 起点重叠），改为在 linker 里**新增 MEMORY 区**直接 reserve。
2. **D-cache 一致性**：D-cache 默认打开（`BSP_SCB_ENABLE_D_CACHE` 见 `board.c:164-166`），AXI-SRAM 默认 cacheable。在 DMA 完成中断里必须 `SCB_InvalidateDCache_by_Addr()` 再读 buffer，沿用参考工程 `main.c:113-117` 的写法。SRAM4 在 D3 域 + BDMA 路径默认 non-cacheable，可省 invalidate；为统一仍做一次（安全无副作用）。
3. **FDCAN 消息 RAM**：H7 FDCAN1/2 共享 10 KB 消息 RAM，CubeMX 默认 `RxFifo0ElmtsNbr=0/TxFifoQueueElmtsNbr=0` 等价于不启用接收/发送，必须显式分配。本计划：RX FIFO0 = 16 elmts × 18B (CAN 经典 8B 数据) = 288 B；TX FIFO = 8 elmts × 18B = 144 B；标准 ID 滤波 = 8；扩展 ID 滤波 = 0。
4. **位时序（500 kbps @ FDCAN_CLK=25 MHz HSE）**：Tq = 1/25MHz = 40 ns，bit time = 2 μs = 50 Tq。取 `Prescaler=2, SJW=1, TS1=17, TS2=7`，实际 = 25M/2/(1+17+7) = **500 kbps**、采样点 72%。CubeMX 默认值（Prescaler=16, TS1=TS2=1）**不要用**，按这里覆盖。
5. **NVIC 路由**：
   - ADC1 DMA：`DMA1_Stream0_IRQHandler`
   - ADC3 BDMA：`BDMA_Channel0_IRQHandler`
   - FDCAN2：`FDCAN2_IT0_IRQHandler`（IT0 = 主中断线，IT1 留作错误，本期不开）
   - 全在 vendor 启动文件 `startup_stm32h750xx.s` 里有 weak 桩，应用层定义同名函数即覆盖；优先级 4-5（低于 RT-Thread 的 `PendSV`/`SysTick`，默认 `__NVIC_PRIO_BITS=4`）。**禁止用 0，否则会阻塞调度**。
6. **CubeMX 重生成自检**：若以后跑 CubeMX 重生代码，`board/CubeMX_Config/Core/Src/main.c` 会被覆盖，但本工程**只编 `system_stm32h7xx.c` + `stm32h7xx_hal_msp.c`**（见 `board/SConscript:14-17`），所以重生不影响 app_drv 实现。但若改了 ADC/FDCAN 引脚或 ADC 通道顺序，要同步更新 `app_drv_adc.c` 内的 init 序列。

## 详细设计

### 1. 链接脚本（`board/linker_scripts/link.lds`）

在 `MEMORY {}` 块加两段：

```ld
RAM_D1 (rw) : ORIGIN = 0x24000000, LENGTH = 512K /* AXI SRAM, DMA1 可达 */
RAM_D3 (rw) : ORIGIN = 0x38000000, LENGTH = 64K  /* SRAM4, BDMA 可达 */
```

在 `SECTIONS {}` 里、`.bss` 之后追加：

```ld
.dma_buffer (NOLOAD) :
{
    . = ALIGN(32);
    *(.dma_buffer)
    *(.dma_buffer.*)
    . = ALIGN(32);
} > RAM_D1

.ram_d3 (NOLOAD) :
{
    . = ALIGN(32);
    *(.ram_d3)
    *(.ram_d3.*)
    . = ALIGN(32);
} > RAM_D3
```

> `board.c:104-119` 的 `init_sram` 会把整个 AXI-SRAM/SRAM4 注册成 memheap，本计划划走的几百字节落在域起始，链接器和 memheap 都不会冲突——但**实施时要把 memheap 的起始地址改为 buffer 之后**（或缩小 size）。具体做法：在 `app_drv_adc.h` 暴露 `__dma_buffer_end_d1` / `__ram_d3_end` 符号，`init_sram` 用它做 base。这一步实施时再细化，先在计划里点名。

`post_build.py` 会从 `MEMORY {}` 里抽 ROM/RAM 大小用作 Flash/RAM 占比展示，新增的 `RAM_D1/RAM_D3` 不在主 RAM 计算口径里，不影响输出。

### 2. `app_drv_adc.h` / `app_drv_adc.c`

**对外 API**（单例，无句柄参数）：

```c
#define ADC_TOTAL_CH  16U

int     app_drv_adc_init(void);                   /* INIT_DEVICE_EXPORT 调用 */
int     app_drv_adc_start(void);                  /* 启动 calibration + DMA + TIM6 */
int     app_drv_adc_stop(void);
int     app_drv_adc_wait(rt_int32_t timeout_ms);  /* 阻塞等下一帧快照 ready */
void    app_drv_adc_get_snapshot(uint16_t out[ADC_TOTAL_CH]); /* 拷贝 16 路 raw */
uint32_t app_drv_adc_raw_to_mv(uint16_t raw);     /* 3300mV / 65535 线性 */
```

**内部状态**：

```c
static ADC_HandleTypeDef hadc1, hadc3;
static DMA_HandleTypeDef hdma_adc1;     /* 注意：board.c:13-14 已有同名 globals，
                                           本驱动改为 static，删 board.c 那两行 */
static DMA_HandleTypeDef hdma_adc3;
static TIM_HandleTypeDef htim6;

static uint16_t adc1_dma_buf[14] __attribute__((section(".dma_buffer"), aligned(32)));
static uint16_t adc3_dma_buf[2]  __attribute__((section(".ram_d3"), aligned(32)));
static uint16_t adc_snapshot[16];
static struct rt_semaphore adc_sem;
```

**通道映射**（与参考工程 `adc_pin_names` 1:1）：

| Idx | Pin | ADC.Channel | Rank |
|---|---|---|---|
| 0-13 | PA6/PC4/PB1/PA7/PC5/PB0/PC0/PC1/PA2/PA3/PA0/PA1/PA4/PA5 | ADC1.CH3/4/5/7/8/9/10/11/14/15/16/17/18/19 | 1-14 |
| 14 | PC3 | ADC3.CH1 | 1 |
| 15 | PC2 | ADC3.CH0 | 2 |

注意 ADC3 的 rank 顺序：`ADC_CHANNEL_1` 是 rank 1（落 `adc3_dma_buf[0]`），`ADC_CHANNEL_0` 是 rank 2（落 `adc3_dma_buf[1]`），所以 snapshot[14]/[15] 对应 PC3/PC2。

**初始化序列**（搬自参考工程 `main.c:245-258`、`MX_ADC1_Init`、`MX_ADC3_Init`、`MX_TIM6_Init`）：

1. `__HAL_RCC_DMA1_CLK_ENABLE()` / `__HAL_RCC_BDMA_CLK_ENABLE()`（MX_DMA_Init / MX_BDMA_Init 等价）
2. NVIC 使能 `DMA1_Stream0_IRQn` / `BDMA_Channel0_IRQn`，优先级 5
3. `MX_ADC1_Init`：ClockPrescaler=`ADC_CLOCK_ASYNC_DIV2`、Resolution=16B、Scan=Enable、NbrOfConversion=14、ExtTrig=`ADC_EXTERNALTRIG_T6_TRGO`/RISING、DataMgmt=`DMA_CIRCULAR`、Overrun=`PRESERVED`；逐通道 `HAL_ADC_ConfigChannel`，SamplingTime=`ADC_SAMPLETIME_1CYCLE_5`、SingleEnded、Offset=None
4. `MX_ADC3_Init`：同上但 NbrOfConversion=2、双通道
5. `MX_TIM6_Init`：PSC=23999、ARR=999、TRGO=Update（10 Hz @ 240 MHz APB1×2）
6. `HAL_ADCEx_Calibration_Start(OFFSET, SINGLE_ENDED)` × 2
7. `HAL_ADC_Start_DMA(&hadc1, adc1_dma_buf, 14)` / `HAL_ADC_Start_DMA(&hadc3, adc3_dma_buf, 2)`
8. `HAL_TIM_Base_Start(&htim6)`

**中断回调**（ADC1 通道多必然后完成，用 ADC1 完成做 batch ready 信号）：

```c
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc) {
    if (hadc->Instance == ADC3) {
        SCB_InvalidateDCache_by_Addr((uint32_t*)adc3_dma_buf, sizeof(adc3_dma_buf));
        memcpy(&adc_snapshot[14], adc3_dma_buf, sizeof(adc3_dma_buf));
    } else if (hadc->Instance == ADC1) {
        SCB_InvalidateDCache_by_Addr((uint32_t*)adc1_dma_buf, sizeof(adc1_dma_buf));
        memcpy(adc_snapshot, adc1_dma_buf, sizeof(adc1_dma_buf));
        rt_sem_release(&adc_sem);
    }
}
```

**IRQ stubs**（应用层覆盖 weak 桩）：

```c
void DMA1_Stream0_IRQHandler(void)   { rt_interrupt_enter(); HAL_DMA_IRQHandler(&hdma_adc1); rt_interrupt_leave(); }
void BDMA_Channel0_IRQHandler(void)  { rt_interrupt_enter(); HAL_DMA_IRQHandler(&hdma_adc3); rt_interrupt_leave(); }
```

### 3. `app_drv_can.h` / `app_drv_can.c`

**对外 API**（单例，对外暴露不透明句柄以满足"单例 + 句柄"语义）：

```c
typedef struct app_drv_can_frame {
    uint32_t id;             /* 11-bit 标准 ID */
    uint8_t  ide;            /* 0=Std, 1=Ext */
    uint8_t  rtr;            /* 0=Data, 1=Remote */
    uint8_t  dlc;            /* 0..8 */
    uint8_t  data[8];
} app_drv_can_frame_t;

typedef struct app_drv_can *app_drv_can_t;  /* 不透明指针 */

app_drv_can_t app_drv_can_instance(void);                          /* 单例 getter */
int           app_drv_can_init(void);                              /* INIT_DEVICE_EXPORT */
int           app_drv_can_send(app_drv_can_t h, const app_drv_can_frame_t *f, rt_int32_t timeout_ms);
int           app_drv_can_recv(app_drv_can_t h, app_drv_can_frame_t *f, rt_int32_t timeout_ms);
int           app_drv_can_set_filter_accept_all(app_drv_can_t h); /* 收所有标准帧到 RX FIFO0 */
```

**内部状态**：

```c
#define CAN_RX_RING_SZ  32

struct app_drv_can {
    FDCAN_HandleTypeDef hfdcan;
    app_drv_can_frame_t rx_ring[CAN_RX_RING_SZ];
    volatile uint16_t   rx_head, rx_tail;
    struct rt_semaphore rx_sem;
    struct rt_mutex     tx_mutex;
};

static struct app_drv_can g_can;   /* 单例实体 */
```

**FDCAN2 初始化**（500 kbps Classic）：

```c
hfdcan.Instance = FDCAN2;
hfdcan.Init.FrameFormat = FDCAN_FRAME_CLASSIC;
hfdcan.Init.Mode = FDCAN_MODE_NORMAL;
hfdcan.Init.AutoRetransmission = ENABLE;
hfdcan.Init.TransmitPause = DISABLE;
hfdcan.Init.ProtocolException = DISABLE;
hfdcan.Init.NominalPrescaler = 2;            /* 25M/2 = 12.5M Tq */
hfdcan.Init.NominalSyncJumpWidth = 1;
hfdcan.Init.NominalTimeSeg1 = 17;
hfdcan.Init.NominalTimeSeg2 = 7;             /* 25 Tq, 500 kbps, 72% 采样点 */
hfdcan.Init.DataPrescaler = 1;               /* Classic 模式忽略 */
hfdcan.Init.DataSyncJumpWidth = 1;
hfdcan.Init.DataTimeSeg1 = 1;
hfdcan.Init.DataTimeSeg2 = 1;
hfdcan.Init.MessageRAMOffset = 0;
hfdcan.Init.StdFiltersNbr = 8;
hfdcan.Init.ExtFiltersNbr = 0;
hfdcan.Init.RxFifo0ElmtsNbr = 16;
hfdcan.Init.RxFifo0ElmtSize = FDCAN_DATA_BYTES_8;
hfdcan.Init.RxFifo1ElmtsNbr = 0;
hfdcan.Init.RxBuffersNbr = 0;
hfdcan.Init.TxEventsNbr = 0;
hfdcan.Init.TxBuffersNbr = 0;
hfdcan.Init.TxFifoQueueElmtsNbr = 8;
hfdcan.Init.TxFifoQueueMode = FDCAN_TX_FIFO_OPERATION;
hfdcan.Init.TxElmtSize = FDCAN_DATA_BYTES_8;
HAL_FDCAN_Init(&hfdcan);
```

启动序列：

1. `HAL_FDCAN_Init`
2. `app_drv_can_set_filter_accept_all`（一个 ID range 滤波 0x000-0x7FF → RX FIFO0）
3. `HAL_FDCAN_ConfigGlobalFilter(REJECT, REJECT, ...)` 关闭非匹配帧
4. `HAL_FDCAN_ActivateNotification(&hfdcan, FDCAN_IT_RX_FIFO0_NEW_MESSAGE, 0)`
5. `HAL_NVIC_SetPriority(FDCAN2_IT0_IRQn, 5, 0); HAL_NVIC_EnableIRQ(FDCAN2_IT0_IRQn);`
6. `HAL_FDCAN_Start(&hfdcan)`

**中断回调**：

```c
void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t flags) {
    if (!(flags & FDCAN_IT_RX_FIFO0_NEW_MESSAGE)) return;
    FDCAN_RxHeaderTypeDef hdr;
    uint8_t data[8];
    while (HAL_FDCAN_GetRxFifoFillLevel(hfdcan, FDCAN_RX_FIFO0) > 0) {
        if (HAL_FDCAN_GetRxMessage(hfdcan, FDCAN_RX_FIFO0, &hdr, data) != HAL_OK) break;
        uint16_t next = (g_can.rx_head + 1) % CAN_RX_RING_SZ;
        if (next == g_can.rx_tail) break;     /* full, drop */
        app_drv_can_frame_t *f = &g_can.rx_ring[g_can.rx_head];
        f->id  = hdr.Identifier;
        f->ide = (hdr.IdType == FDCAN_EXTENDED_ID);
        f->rtr = (hdr.RxFrameType == FDCAN_REMOTE_FRAME);
        f->dlc = hdr.DataLength;
        memcpy(f->data, data, 8);
        g_can.rx_head = next;
    }
    rt_sem_release(&g_can.rx_sem);
}

void FDCAN2_IT0_IRQHandler(void) {
    rt_interrupt_enter();
    HAL_FDCAN_IRQHandler(&g_can.hfdcan);
    rt_interrupt_leave();
}
```

**`_send` 实现**：拿 `tx_mutex` → 构造 `FDCAN_TxHeaderTypeDef` → `HAL_FDCAN_AddMessageToTxFifoQ` → 释放 mutex。若 TX FIFO 满，按 timeout 轮询 `HAL_FDCAN_GetTxFifoFreeLevel`（轮询周期 1 ms，简单够用；后续按需改为 TX-complete IRQ + 信号量）。

**`_recv` 实现**：`rt_sem_take(timeout_ms)` → 关 IRQ → 从 ring tail 拷出一帧 → 开 IRQ → 返回。

### 4. `applications/app_drv/SConscript`

```python
from building import *
cwd  = GetCurrentDir()
src  = Glob('*.c')
group = DefineGroup('AppDrv', src, depend = [''], CPPPATH = [cwd])
Return('group')
```

### 5. `applications/main.c`

直接用 `INIT_DEVICE_EXPORT(app_drv_adc_init); INIT_DEVICE_EXPORT(app_drv_can_init);` 让 RT-Thread 在 components_init 阶段自动调，`main` 不动——单例驱动天然就该在 components init 期间起来。

### 6. MSH 测试命令（写在各自驱动的 .c 末尾）

```c
static int cmd_adc_dump(int argc, char **argv) {
    app_drv_adc_wait(1000);
    uint16_t s[16];
    app_drv_adc_get_snapshot(s);
    for (int i = 0; i < 16; i++) rt_kprintf("ch%02d: raw=%5u  mv=%4u\n", i, s[i], app_drv_adc_raw_to_mv(s[i]));
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_adc_dump, adc_dump, dump latest 16-ch ADC snapshot);

static int cmd_can_send(int argc, char **argv);  /* can_send <hexid> <b0..b7> */
MSH_CMD_EXPORT_ALIAS(cmd_can_send, can_send, send one CAN frame);

static int cmd_can_sniff(int argc, char **argv) {
    int n = (argc > 1) ? atoi(argv[1]) : 10;
    app_drv_can_frame_t f;
    while (n-- > 0 && app_drv_can_recv(app_drv_can_instance(), &f, 5000) == RT_EOK) {
        rt_kprintf("ID=0x%03X DLC=%u  ", f.id, f.dlc);
        for (int i = 0; i < f.dlc; i++) rt_kprintf("%02X ", f.data[i]);
        rt_kprintf("\n");
    }
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_can_sniff, can_sniff, sniff CAN frames);
```

参考 `MSH_CMD_EXPORT_ALIAS` 见 `libraries/HAL_Drivers/drv_common.c`。

## 实施步骤（建议顺序）

1. 改 `board/linker_scripts/link.lds`，加 `RAM_D1/RAM_D3` + `.dma_buffer/.ram_d3`；改 `board/board.c::init_sram` 把 AXI/SRAM4 的 base/size 让出对应字节（或直接保留 memheap 全量，验证不冲突——本期建议保守让出 1 KB）。`./run.sh rebuild`，核对 `post_build.py` 输出 RAM 占比正常。
2. 删除 `board/board.c:13-14` 的 `hdma_adc1/hdma_adc3` 全局声明（避免和 app_drv_adc.c 里的 static 冲突）。
3. 创建 `applications/app_drv/{SConscript, app_drv_adc.h, app_drv_adc.c}`，先把 ADC 跑通；用 `adc_dump` 命令验证。
4. 创建 `app_drv_can.h / app_drv_can.c`；用 `can_sniff` 接一台 CAN 分析仪验证 500 kbps 收发。
5. 把 `INIT_DEVICE_EXPORT` 接上。

## Verification

**编译期**：
```
./run.sh rebuild         # 主 app，期望 build/rt-thread.bin 生成成功
./run.sh app-flash       # 烧到 QSPI
./run.sh reset           # 启动
```

**运行期**：
- `msh > adc_dump` → 16 路输出，未接线通道在 1.5 cycle 短采样下可能漂动；接一个分压电阻到 PA0（ADC1_CH16，对应 idx 10），应稳定读到对应 mV。
- `msh > can_sniff 50` → 用 CANalyst-II 或类似在 PB12/PB13 端打 500 kbps 经典帧，能看到正确 ID/DLC/数据。
- `msh > can_send 0x123 11 22 33 44` → 分析仪侧能抓到对应帧、CRC OK、无 ACK 错误（前提：总线上至少有第二个节点应答 ACK，或开 `FDCAN_MODE_BUS_MONITORING` 临时单机测试发送）。
- 用 `arm-none-eabi-size build/rt-thread.elf` 看 `.dma_buffer` / `.ram_d3` 段大小符合预期（≈28 B / 4 B，对齐到 32 B）。
- 用 J-Link `mem32 0x24000000 16` 检查 DMA 完成后 `adc1_dma_buf` 内容随触发更新（每 100 ms 一次）。

**失败兜底**：若 ADC 一直读 0xFFFF → 检查 PLL2 是否实际产生 ADC clock（`board.c::PeriphCommonClock_Config` 的 PLL2N=12 是否合法）+ DMA stream IRQ 是否真打上来（J-Link `mem32 0xE000E200` 看 NVIC_ISER）。若 CAN 不通 → 先看 `HAL_FDCAN_GetProtocolStatus` 的 LEC（Last Error Code）、再核 bit timing。

## 待实施前再确认（次要项）

- 滤波器：本期默认"接收所有标准 ID 到 FIFO0"。如果业务需要按 ID 段过滤，告知 ID 范围即可在 `set_filter_accept_all` 之外再加 API。
- 总线 ACK：如果一开始只有这一台 MCU 上电、无对端，发送会一直 ACK 错误并占满 TX FIFO。建议先用分析仪侧充当第二节点；或临时把 `Mode = FDCAN_MODE_BUS_MONITORING` 验证收路。
