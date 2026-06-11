# test/ — 闭环调试 / 验收套件

本目录是面向 PC 主机的闭环测试工具，通过 J-Link SWD、USB-TTL 串口、ZQWL UCANFD-100C CAN box 与目标板互动。所有脚本只依赖 Python 3 标准库，无 `pyserial` 等外部包。

## 目录约定

```
test/
├── lib/                      # 可复用模块（不直接执行）
│   ├── jlink.py              # JLinkExe 包装：reset_run / halt_and_regs / read32_many
│   ├── serial_term.py        # 串口包装：Term.read / send_line / expect
│   └── zqwl_can.py           # ZQWL 协议封装：ZqwlCan.send / recv / raw_drain
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

## 与 MSH 命令的约定

`serial_term.Term.send_line()` 默认按 25 字符/秒发送（`CHAR_DELAY_S = 0.04`），原因是 finsh shell 在 115200 全速喂入时单字符 ring 会丢字。新 case 复用此 API、不要直接 `os.write(fd, b"long_cmd\n")`。

## 协议参考

- ZQWL UCANFD 二次开发协议见 `/home/huan/tool/ZQWL-UCANFD-100C/UCANFD/MANUAL/ZQWL-USBCANFD二次开发通讯协议_V1.05.pdf`，本目录 `lib/zqwl_can.py` 只内联了用得到的部分（config 帧 `49 3B … 45 2E`、CAN 帧 `5A … A5`、心跳过滤）。
- J-Link SWD 调试细节见 `.agent/workflow/stm32h7-jlink-gdb-serial-closed-loop-debug.md`。
