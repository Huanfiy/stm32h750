# ZQWL UCANFD-100C 卡进「1 USB 包/秒」节流态 —— 仅断电可恢复

## 状态

**已恢复（2026-06-12，拔插 USB 断电重启）。** 根因位于盒子固件内部、宿主侧不可达；
本文记录症状识别、已证伪的恢复手段与复发时的处置，避免下次重走一遍排查。
测试侧的耐受措施（条件等待上限、`test_can.test_rx` 陈旧帧重试）已随 2026-06-11/12
的测试套件优化落地。

## 现象

- `test_can` tx 方向（MCU → 盒 → PC）突然需要 4~5 秒，健康基线 <20 ms；
  整个套件从 ~29 s 膨胀到 ~65 s，但**仍然全绿** —— 变慢本身就是信号。
- `can_sniff` 抓到陈旧业务帧（如 `ID=0x120 01 4D…`/`02 4D…`，即更早 case
  `test_ag_monitor_power_log` 的 batch=77 START/STOP 控制帧）：盒子在通道重开时
  把内部队列里的旧帧重放到总线上，每次重开吐一帧。
- 心跳包自报状态字节 `0x20`（CAN0 开、总线正常）—— 盒子不自知异常。

## 定位

对 `/dev/ttyACM1` 逐 USB chunk 打时间戳（向 msh 发 `can_send`，记录盒子上行）：

```
t+0.012s  49 3B 40 52 …   ← 几秒前 _init 发的设备信息请求的响应，现在才到
t+0.539s  5A FF … A5      ← 心跳
t+1.539s  49 3B 42 57 …   ← 波特率配置响应
t+2.539s  49 3B 44 57 …   ← 系统控制响应
t+3.539s  5A 04 00 00 00 01 23 DE AD BE EF A5   ← 等的数据帧，排在第 5 个槽
```

所有上行包（配置响应、心跳、数据帧）进同一个 FIFO，**严格 1 包/秒**出队;
帧延迟 = 队列里排在它前面的包数 × 1 s。配置响应是纯 USB 往返、与 CAN 总线无关，
也被同样延迟 —— 据此排除总线侧问题，坐实盒子 USB 上行调度病态。
厂商手册（`ZQWL-USBCANFD二次开发通讯协议_V1.05.pdf`）明确健康行为：数据帧即时转发、
上位机发包时心跳立即返回；空闲时心跳才是 1 s 一条。

触发条件未复现成功（进入病态的时间窗内只有 MCU 多次 J-Link 复位 + 常规 CAN 收发），
不排除「MCU 复位瞬间盒子有 pending TX」之类的边界。

## 已证伪的恢复手段（全部无效，不必再试）

| 手段 | 结果 |
|---|---|
| 重发全套配置（0x42 波特率 + 0x44 应用/开通道） | 无效，响应照样排队 1 包/秒 |
| 通道软关开（0x44 data[2]=0 → =1） | 无效 |
| 厂商系统复位命令（0x44 **data[1]=0x01**） | 无效，设备甚至未重新枚举（疑似未执行） |
| `USBDEVFS_RESET` ioctl（需 sudo） | 重新枚举但病态保留 |
| sysfs `authorized` 0→1 强制重枚举 | 病态保留 |
| **拔插 USB（断 VBUS 电）** | **恢复，延迟回到 12~18 ms** |

USBDEVFS_RESET / authorized 不切断 VBUS，盒子 MCU 不掉电，状态存活——只有物理断电有效。

## 复发时的快速判定

```python
# 量化转发延迟：msh can_send 后计时收到 0x123 帧（健康 <0.1 s）
import sys, time, os, select
sys.path.insert(0, 'test')
from lib import serial_term, zqwl_can
zq = zqwl_can.ZqwlCan(); term = serial_term.Term()
zq.flush_input(); term.flush_input(); t0 = time.time()
term.send_raw(b"can_send 0x123 DE AD BE EF\r\n")
end = t0 + 6.0; seen = False
while time.time() < end and not seen:
    r, _, _ = select.select([zq.fd], [], [], end - time.time())
    if not r: break
    seen = any(c == 0x123 for c, _, _ in zqwl_can.parse_frames(os.read(zq.fd, 4096)))
print(f"latency {time.time()-t0:.3f}s matched={seen}")
```

> 0.1 s 量级 = 健康；4~5 s 且逐秒出包 = 节流态，直接拔插盒子，别浪费时间试软恢复。

## 顺带勘误（已修入 `test/lib/zqwl_can.py` 注释）

0x44 系统控制包 data 区按手册实义：`data[0]`=生效参数（**0x01 = 生效并写入 flash**，
旧注释"不写 flash"有误）；`data[1]`=系统复位（0x01 = 复位设备）；`data[2]/[3]`=CAN0/CAN1 开关。

## 通用教训

1. **闭环测试套件"全绿但变慢"要当故障信号看**：条件等待的超时上限会吸收链路退化，
   PASS/FAIL 不反映链路健康，耗时才反映。`run_all.py` 汇总表和 `test_can_protocol` 的
   `[time]` 打印就是为此存在的基线。
2. 排查转发延迟时，**用与总线无关的纯 USB 往返（配置请求/响应）做对照**，
   一步区分"总线问题"和"适配器 USB 侧问题"。
3. 外设维持在病态时自报状态可能完全正常（心跳 0x20），**不要拿设备自检结果排除设备本身**。
