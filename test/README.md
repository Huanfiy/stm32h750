# test/ — 闭环调试 / 验收套件

本目录是面向 PC 主机的闭环测试工具，通过 J-Link SWD、USB-TTL 串口、ZQWL UCANFD-100C CAN box 与目标板互动。所有脚本只依赖 Python 3 标准库，无 `pyserial` 等外部包。

## 目录约定

```
test/
├── lib/                      # 可复用模块（不直接执行）
│   ├── jlink.py              # JLinkExe 包装：reset_run / halt_and_regs(pre_cmds) / read32_many
│   ├── serial_term.py        # 串口包装：Term.read / send_line / send_raw / expect / flush_input
│   └── zqwl_can.py           # ZQWL 协议封装：ZqwlCan.send / recv / raw_drain / flush_input / pending_raw
├── cases/                    # 独立可执行测试用例（标准退码：0=PASS, 1=FAIL, 77=SKIP）
│   ├── test_swd.py           # J-Link halt + PC 落在 QSPI XIP 区
│   ├── test_boot.py          # 重启后串口看到 RT-Thread banner + msh 提示符
│   ├── test_sd.py            # SDMMC 枚举、容量识别、无 hard fault
│   ├── test_fs.py            # FATFS 自动挂载 + 写入/复位/读回
│   ├── test_adc.py           # msh `adc_dump` 输出 16 路、值在量程内
│   ├── test_can.py           # ZQWL ↔ MCU 双向收发，ID + 数据完全一致
│   ├── test_can_protocol.py  # 业务 CAN 协议配置/绑定/开始 ACK + 周期上报
│   ├── test_ag_monitor_power_log.py # 低电流事件、断电、100 Hz SD 日志
│   ├── test_pwr_en.py        # 14 路 GPIO-owned PWR_EN 物理 ODR 校验
│   ├── test_msh_history.py   # MSH 上键历史召回
│   ├── test_can_diagnostics.py # 手动诊断：读取 FDCAN2 错误寄存器
│   └── test_can_user.py      # 手动工具：交互式发送 CAN 帧
└── run_all.py                # 按顺序跑默认验收 case，输出汇总表
```

`run_all.py` 默认执行 `CASE_ORDER` 中的 10 个闭环验收用例，不执行手动诊断/交互工具。

## 硬件接线前置

- J-Link 通过 SWD 接到目标板，`JLinkExe` 在 PATH，device = `STM32H750VB`、speed = 4 MHz；
- USB-TTL 接到 USART1 (`PB14`/`PB15`)，默认节点 `/dev/ttyUSB0`、`115200 8N1`；
- ZQWL UCANFD-100C 通过 USB-CDC 出现为 `/dev/ttyACM*`（默认假设 `/dev/ttyACM1`，可在 `lib/zqwl_can.py:DEFAULT_DEV` 改），CAN_H/CAN_L 与目标板 `PB13`/`PB12` 并联，两端 120 Ω 终端电阻。

`lib/*.py` 内置 `device_present()` / `have_jlink()` 探针，硬件缺失时对应 case 退码 77 (SKIP)，不会让整套测试 FAIL。

## 运行

```
python3 test/run_all.py             # 全部 case + 汇总
python3 test/cases/test_adc.py      # 跑单个 case
python3 test/cases/test_can_protocol.py  # 业务 CAN 协议闭环
python3 test/cases/test_can_diagnostics.py  # CAN 无收发时看 Bus-Off / TEC / LEC
```

## 写新 case 的模板

新 case 放到 `cases/`，命名 `test_<feature>.py`，按下面骨架写：

```python
#!/usr/bin/env python3
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from lib import serial_term  # 或 jlink / zqwl_can

EXIT_PASS, EXIT_FAIL, EXIT_SKIP = 0, 1, 77

def main() -> int:
    if not serial_term.device_present():
        print("SKIP: serial missing")
        return EXIT_SKIP
    with serial_term.Term() as term:
        term.send_line("my_cmd")
        buf, ok = term.expect(rb"OK", timeout=2.0)
        return EXIT_PASS if ok else EXIT_FAIL

if __name__ == "__main__":
    sys.exit(main())
```

把 case 文件名加入 `run_all.py::CASE_ORDER` 即可纳入全量跑。

## 等待纪律（写 case 必读）

case 的等待一律**事件驱动、超时只是上限**，不要按最坏情况睡满固定时长：

- 串口侧等**精确日志行**：`term.expect(rb"\[FS\].*mounted at", 5.0)`、`expect(rb"SD card capacity…")`，
  而不是 `term.read(4.0)` 盲读。固件里有确切完成信号的，等那一行。
- CAN 侧等**解码后的条件**：照抄 `test_can_protocol.py::_wait_until(zq, rx, cond, timeout)` ——
  `recv()` 每到一帧就醒一次，条件成立即返回，超时只兜底。把"缺什么"写成
  `_missing(rx) -> list[str]`，等待谓词和失败报文共用同一函数。
- 清残留用 `flush_input()`（`tcflush` 即时清空），不要 `read(0.3)` / `raw_drain(0.8)` 定时排空。
- **固定时长窗口只留给负向检查**（"STOP 后不得再有上报"、"禁用通道必须沉默"）——那是语义，
  不是浪费；正向等待出现固定 sleep 都应视为待修。

## 与 MSH 命令的约定

向 msh 发命令有两条路径：

- `Term.send_line()` —— 按 25 字符/秒逐字符发送（`CHAR_DELAY_S = 0.04`），任何长度的命令都安全。
- `Term.send_raw(line + b"\r\n")` 整行突发 + `expect` 提示符同步 —— **仅限含 CRLF ≤ 16 字节的短命令**
  （H7 UART RX FIFO 深度，见 `.agent/fixed/2026-06-02-msh-arrow-history-uart-rx-byte-loss-no-hw-fifo.md`；
  `test_msh_history.py` 的 30/30 即此路径的回归背书）。批量短命令（如 `test_pwr_en.py` 的 28 条）
  用这条路径可把 12 秒级的逐字符耗时压到 1 秒内。

新 case 复用这两个 API、不要直接 `os.write(fd, b"long_cmd\n")`。

## 已知硬件故障模式：ZQWL 盒「1 包/秒」节流态

ZQWL UCANFD-100C 偶发进入一种病态：**所有 USB 上行（配置响应、心跳、数据帧）排进 FIFO，
严格每秒只放一个包**，同时通道重开时会把队列里的陈旧帧重放到总线上。症状：

- `test_can` 的 tx 方向突然要 4~5 秒（健康时 <20 ms）；`test_can_protocol` 的 `[time]` 打印整体膨胀；
- `can_sniff` 抓到 `0x1xx` 的陈旧业务帧（之前 case 的 START/STOP 等）；
- 心跳自报状态正常（0x20 = CAN0 开、总线正常），盒子不自知。

重下发配置、通道关开、厂商系统复位命令（0x44 data[1]=1）、`USBDEVFS_RESET`、sysfs 重枚举
**均无效，唯一恢复手段是拔插 USB 断电重启**。完整定位过程见
`.agent/fixed/2026-06-12-zqwl-canfd-usb-1pps-throttle-state.md`。
套件对此有一定耐受（条件等待的超时上限会吸收延迟、`test_can.test_rx` 有陈旧帧重试），
病态下通常仍全绿、只是变慢——所以**变慢本身就是该故障的信号**。

## 协议参考

- ZQWL UCANFD 二次开发协议见 `/home/huan/tool/ZQWL-UCANFD-100C/UCANFD/MANUAL/ZQWL-USBCANFD二次开发通讯协议_V1.05.pdf`，本目录 `lib/zqwl_can.py` 只内联了用得到的部分（config 帧 `49 3B … 45 2E`、CAN 帧 `5A … A5`、心跳过滤）。
- J-Link SWD 调试细节见 `.agent/workflow/stm32h7-jlink-gdb-serial-closed-loop-debug.md`。
