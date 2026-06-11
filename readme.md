# 智能 B 柱产测老化工装固件

面向智能 B 柱模块老化产测工位的下位机固件。单工位保留 16 路物理编号，其中 14 路具备电源控制能力；固件按 100 Hz 采集电流，将可控通道监控数据落盘至 SD 卡，并通过 CAN 周期性向上位机上报各路状态。

运行平台为 STM32H750VBT6（Cortex-M7 @ 480 MHz）+ RT-Thread 5.2.0 BSP。

## 1. 覆盖范围与边界

为避免误读固件当前能力，先明确"已实现"与"尚未接入"的分界。

已实现并通过真机闭环测试的底层能力：

- 16 路电流通道 ADC 采集（100 Hz 整帧快照）；
- CAN（FDCAN2，Classic 500 kbps）双向收发；
- 老化测试业务 CAN 协议闭环：配置 `0x100` ACK、通道绑定 `0x110/0x111`、开始/停止/复位 `0x120` ACK、运行周期上报 `0x200`、低/高电流事件 `0x210`、越限切断对应 PWR_EN、100 Hz 监控数据落盘；
- SD 卡 FATFS 自动挂载至 `/`；
- TIM1 板级 PWM 输出（PE14）；
- finsh/MSH 命令行（USART1）。

当前边界：

- MCU 侧已按上位机命令执行 14 路可控供电通道的启动、100 Hz 监控、越限断电、事件上报和 SD 落盘；MES 结果关联仍由上位机侧闭环；
- ADC 原始值 → 实际电流（mA）已按 INA240A2（50 V/V）+ 50 mΩ 分流电阻换算；当前按 INA240 REF = 0 mV 处理，若硬件 REF 存在偏置，需要同步调整 `INA240_REF_MV`。

## 2. 硬件平台

| 项目 | 参数 |
| --- | --- |
| MCU | STM32H750VBT6，Cortex-M7，480 MHz |
| 内部 Flash | 128 KB @ `0x08000000`（仅存 bootloader） |
| 外部 Flash | W25Q64 8 MB QSPI @ `0x90000000`（主程序 XIP 运行） |
| 主 RAM | DTCM 128 KB @ `0x20000000`（`.data`/`.bss`/栈） |
| 其他 RAM | AXI-SRAM / SRAM1~4 / BACKUP 以命名 `rt_memheap` 形式注册 |
| 调试/烧录 | J-Link SWD，设备名 `STM32H750VB` |

## 3. 固件构成（两段式）

两段固件独立编译、独立烧录，互不覆盖。

- **Bootloader**（`bootloader/`，独立 `Makefile`）：约 17 KB，驻留内部 Flash。复位后初始化 QSPI W25Q64 至 1-4-4 内存映射模式，校验 JEDEC ID 与应用栈顶/入口地址后跳转至 `0x90000000`。
- **主程序**（根 `SConstruct`）：完整 RT-Thread 内核 + ST HAL + finsh，驻留外部 QSPI 并以 XIP 方式运行。

详细的内存布局、MPU 配置、QSPI 烧录算法等工程细节见 `CLAUDE.md`。

## 4. 对外接口与通道约定

物理通道编号保持 `1..16`，其中 `PWR_EN7/PD2` 被 SDMMC1_CMD 占用，`PWR_EN15/PB2` 被 QSPI/XIP 时钟线占用。业务协议只启动和上报 14 路可控供电通道：`1..6`、`8..14`、`16`。

| 业务通道 | PWR_EN 引脚 | ADC 快照 | ADC 引脚/通道 | 协议状态 |
| --- | --- | --- | --- | --- |
| CH1 | PE1 | `snapshot[0]` | PA6 / ADC1_INP3 | 可控、上报 |
| CH2 | PB9 | `snapshot[1]` | PC4 / ADC1_INP4 | 可控、上报 |
| CH3 | PB7 | `snapshot[2]` | PB1 / ADC1_INP5 | 可控、上报 |
| CH4 | PB5 | `snapshot[3]` | PA7 / ADC1_INP7 | 可控、上报 |
| CH5 | PB3 | `snapshot[4]` | PC5 / ADC1_INP8 | 可控、上报 |
| CH6 | PD6 | `snapshot[5]` | PB0 / ADC1_INP9 | 可控、上报 |
| CH7 | PD2 | `snapshot[6]` | PC0 / ADC1_INP10 | 保留，不控制/上报 |
| CH8 | PD4 | `snapshot[7]` | PC1 / ADC1_INP11 | 可控、上报 |
| CH9 | PE7 | `snapshot[8]` | PA2 / ADC1_INP14 | 可控、上报 |
| CH10 | PE9 | `snapshot[9]` | PA3 / ADC1_INP15 | 可控、上报 |
| CH11 | PE13 | `snapshot[10]` | PA0 / ADC1_INP16 | 可控、上报 |
| CH12 | PE11 | `snapshot[11]` | PA1 / ADC1_INP17 | 可控、上报 |
| CH13 | PE12 | `snapshot[12]` | PA4 / ADC1_INP18 | 可控、上报 |
| CH14 | PE10 | `snapshot[13]` | PA5 / ADC1_INP19 | 可控、上报 |
| CH15 | PB2 | `snapshot[15]` | PC2 / ADC3_INP0 | 保留，不控制/上报 |
| CH16 | PE8 | `snapshot[14]` | PC3 / ADC3_INP1 | 可控、上报 |

### 4.1 电流采集（16 路 ADC）

- 采集器：ADC1（14 路）+ ADC3（2 路），16-bit 分辨率，单端输入；
- 触发：TIM6 TRGO 硬件触发，整帧扫描率 **100 Hz**；DMA 循环搬运，单帧完成由信号量通知，对外提供原子快照；
- 通道 → 引脚映射（索引 0~15）：

  | idx | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
  | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
  | pin | PA6 | PC4 | PB1 | PA7 | PC5 | PB0 | PC0 | PC1 | PA2 | PA3 | PA0 | PA1 | PA4 | PA5 | PC3 | PC2 |

- 应用接口（`applications/app_drv/app_drv_adc.h`）：
  - `app_drv_adc_wait(timeout_ms)` —— 阻塞至下一整帧就绪；
  - `app_drv_adc_get_snapshot(out[16])` —— 取 16 路原始码值快照；
  - `app_drv_adc_raw_to_mv(raw)` —— 原始码值换算为 mV（基准 3300 mV，满量程 65535）。
  - `app_drv_adc_raw_to_current_ma(raw)` —— 按 INA240A2（50 V/V）+ 50 mΩ 分流电阻换算为 mA。
- 业务通道映射以本节上方 16 路表为准；ADC3 硬件扫描顺序保持 `PC3/ADC3_INP1` 后 `PC2/ADC3_INP0`，业务层将 `CH16` 绑定到 `snapshot[14]`。
- 调试命令：`adc_dump` —— 按业务通道打印 `chn / pwr_en / en_pin / adc_ch / adc_pin / raw / ma`；`pwr_en=-` 表示该通道无 GPIO-owned PWR_EN。
- 电源调试命令：`pwr_en all 0|1` 按原始电平批量拉低/拉高，`pwr_en all en|dis` 按 PWR_EN 逻辑批量使能/关闭。
- PWR_EN 为低有效：`enable` 输出低电平，`disable` 和上电默认状态输出高电平。

### 4.2 CAN 上行

- 控制器：FDCAN2，Classic CAN，**500 kbps**（基于 25 MHz HSE，采样点 72%）；
- 引脚：`PB12`（RX）/ `PB13`（TX）；
- 接收：全通滤波 + 中断填环形缓冲（深度 32），`app_drv_can_recv()` 阻塞取帧；
- 发送：互斥保护 + TX FIFO 满时按 ms 轮询重试，`app_drv_can_send()` 支持超时；
- 应用接口见 `applications/app_drv/app_drv_can.h`；
- 调试命令：`can_send <hexid> [b0..b7]`、`can_sniff [n]`。

### 4.3 老化测试业务 CAN 协议

业务协议实现位于 `applications/app_proto.c`，协议字段以顶层仓 `docs/can_protocol.md` 为准。

- Host → MCU：
  - `0x100` 基础配置，下位机返回 `0x180 ref=0x00`；
  - `0x110` 通道绑定头，下位机返回 `0x180 ref=0x10`；
  - `0x111` SN 分片，首版不逐帧 ACK；
  - `0x120` 测试控制命令，下位机返回 `0x180 ref=0x20`。
- MCU → Host：
  - `0x180` ACK/NACK；
  - `0x200` 周期数据上报，电流字段填 INA240A2 + 50 mΩ 换算后的 mA，ADC raw 字段填原始采样值；
  - `0x210` 异常事件，当前在电流低于/高于配置窗口时每通道发送一次事件并在 `0x200` 中带错误码。
- `CH07`/`CH15` 的绑定请求返回 NACK；开始命令中的这两位会被过滤，不参与供电、监控、上报和落盘。
- 调试命令：`ag_proto` 打印协议配置、运行状态和计数器；`ag_proto ack` 手动发送一帧配置 ACK 便于 CAN 侧抓包。

### 4.4 数据落盘（SD 卡）

- 文件系统：FATFS/ELM，开机由 `fs_mnt` 线程异步等待 `sd0` 枚举完成后挂载至 `/`；
- 卡缺失或挂载失败仅打印告警，不阻塞启动与 msh；
- 测试开始后按批次写入 `/aglog/Bxxxx.CSV`，字段为 `elapsed_ms,ch,current_ma,adc_raw,state,error`；采样节拍为 100 Hz，停止、复位或完成时关闭文件并 `fsync`。

### 4.5 调试串口与命令行

- USART1：`PB14`(RX)/`PB15`(TX)，**115200 8N1**，承载 RT-Thread finsh/MSH；
- 已注册业务命令：`adc_dump` / `can_send` / `can_sniff` / `ag_proto` / `pwr_en` / `pwm_info` / `pwm_duty` / `pwm_high`。

### 4.6 PWM 输出

- TIM1 CH4 @ `PE14`，默认 **50 Hz**（20 ms 周期），上电默认高电平 1.5 ms；
- 接口（`applications/app_drv/app_drv_pwm.h`）：`set_duty(permille)`（0~1000 对应 0~100.0%）、`set_high_time(ns)`；
- 调试命令：`pwm_info`、`pwm_duty <permille>`、`pwm_high <us>`。

## 5. 构建与烧录

所有动作经 `run.sh` 触发，不直接调用 `scons`。

前置环境变量：

- `RTT_ROOT` —— RT-Thread 内核检出路径（本机 `~/SDK/rt-thread`）；
- `RTT_EXEC_PATH` —— `arm-none-eabi-*` 工具链 bin 目录；
- `BUILD_MODE` —— `Debug`（默认，`-O0 -g`）或 `Release`（`-O2 -DNDEBUG`）；
- `JLinkExe` 在 PATH；首次使用须先安装自定义 J-Link 烧录算法：`make -C tools/flashloader install`。

首次端到端烧录（两段固件均需写入）：

```sh
./run.sh rebuild-flash         # 编译并烧录 bootloader + 主程序
./run.sh reset                 # 复位，bootloader 跳入主程序
```

常用命令：

```sh
./run.sh build                 # 编译 bootloader + 主程序
./run.sh flash                 # 烧录 bootloader 至 0x08000000，主程序至 0x90000000
./run.sh build-app             # 仅编译主程序，生成 .vscode/compile_commands.json
./run.sh flash-app             # 仅烧录主程序至 0x90000000
./run.sh build-bl              # 仅编译 bootloader
./run.sh flash-bl              # 仅烧录 bootloader 至 0x08000000
./run.sh <cmd> -v              # 显示完整 scons/make 输出
```

## 6. 测试与验收

`test/` 为 PC 主机侧闭环测试套件，通过 J-Link SWD + USB-TTL 串口 + ZQWL UCANFD-100C CAN box 驱动真机。仅依赖 Python 3 标准库。

```sh
python3 test/run_all.py            # 顺序执行全部 case 并输出汇总
python3 test/cases/test_adc.py     # 单独执行某个 case
python3 test/cases/test_can_diagnostics.py  # CAN 物理层诊断
python3 test/cases/test_can_protocol.py     # 业务 CAN 协议闭环
```

用例退码遵循 autotools 约定：`0=PASS`、`1=FAIL`、`77=SKIP`。硬件缺失时对应 case 自动 SKIP，不会让整套 FAIL。

本仓库约定：每项新功能/驱动须随附 `test/cases/` 下的真机闭环用例，且该用例须在实现提交前通过；无法闭环测试的项需在提交说明中以 `test-exempt: <原因>` 显式标注。接线前置与新用例模板见 `test/README.md`。

## 7. 目录结构

```
.
├── applications/
│   ├── main.c                 # 应用入口（当前为占位）
│   └── app_drv/               # 板级业务驱动
│       ├── app_drv_adc.[ch]   # 16 路电流采集
│       ├── app_drv_can.[ch]   # CAN 收发
│       ├── app_drv_fs.c       # SD 卡自动挂载
│       └── app_drv_pwm.[ch]   # PWM 输出
├── bootloader/                # 内部 Flash bootloader（独立 Make 构建）
├── tools/flashloader/         # J-Link QSPI 烧录算法（独立 Make 构建）
├── board/                     # BSP 板级配置、链接脚本、CubeMX 工程
├── libraries/                 # ST HAL + RT-Thread STM32 驱动 shim
├── test/                      # PC 侧闭环测试套件
├── run.sh                     # 构建/烧录/调试统一入口
└── CLAUDE.md                  # 工程内部细节与排障记录索引
```

## 8. 关键约束与排障

- bootloader、主程序、flashloader 为三套独立构建，**不得**并入根 `SConstruct`；
- J-Link 设备名在 CLI 侧统一为 `STM32H750VB`，自定义烧录算法 XML 须沿用同名，否则 H7 连接序列被跳过、报 "Failed to power up DAP"；
- 已归档的真机问题根因（启动栈溢出、bootloader HardFault、QSPI 烧录算法、SD 卡上电与 D-Cache 等）见 `.agent/fixed/`，进入相近问题排查前先行查阅；
- 完整内存布局、MPU、QSPI XIP、烧录算法等工程细节以 `CLAUDE.md` 为准。
