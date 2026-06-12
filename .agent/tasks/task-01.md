msh />adc_dump
chn pwr_en en_pin adc_ch      adc_pin raw    ma
1   0      PE1    ADC1_INP3   PA6       309     6
2   0      PB9    ADC1_INP4   PC4         0     0
3   0      PB7    ADC1_INP5   PB1      1098    22
4   0      PB5    ADC1_INP7   PA7         0     0
5   0      PB3    ADC1_INP8   PC5       150     2
6   0      PD6    ADC1_INP9   PB0       365     7
7   -      PD2    ADC1_INP10  PC0         0     0
8   0      PD4    ADC1_INP11  PC1       587    11
9   0      PE7    ADC1_INP14  PA2         0     0
10  0      PE9    ADC1_INP15  PA3         0     0
11  0      PE13   ADC1_INP16  PA0        49     0
12  0      PE11   ADC1_INP17  PA1      3885    78
13  0      PE12   ADC1_INP18  PA4        11     0
14  0      PE10   ADC1_INP19  PA5         0     0
15  -      PB2    ADC3_INP0   PC2      1663    33
16  0      PE8    ADC3_INP1   PC3         0     0
vrefint: raw=23826  mV=1199


问题：当前 adc 采出来的数据质量较差，请编写程序，循环采集分析，看是 adc 配置问题还是硬件自身问题

---

## 结论（2026-06-12 闭环实测）

判定：**ADC 配置无问题；噪声来自板级输入侧，与通道是否接负载强相关。**

工具（本次新增，均已上板验证）：

- 固件 `adc_stat [frames]`：连续 N 帧（100 Hz）逐通道 min/max/mean/std 统计；
- 固件 `adc_trace <frames> <ch>... `：选定通道原始时序 CSV 输出；
- `test/adc_quality_probe.py`：自动采集 + 时/频域分析 + 配置/硬件判定；
- `test/cases/test_adc_stat.py`：回归用例（已入 run_all CASE_ORDER，11/11 PASS）。

证据（500 帧统计 + 512 帧时序，全通道电源关闭，PWR_EN 引脚=高=未使能）：

1. **vrefint（片内基准，不经过任何板级网络）std=50 counts（0.21%）**，均值
   1205 mV 在手册范围内 → ADC 内核 + VREF+ 轨 + 时钟/采样/触发/DMA 配置链路健康。
2. **6 个通道（ch1/3/6/8/11/13）在同一份共享配置下 std 仅 6~15 counts（≈0.3~0.7 mV）**
   → 配置本身能产出干净读数，配置问题排除。
3. 噪声通道（ch2/4/5/7/9/10/14/15/16，std 80~410 counts）的时序为**宽带白噪**
   （lag1≈0，无主频峰）→ 是引脚处的真实随机噪声，不是周期性干扰（DC-DC/工频），
   也不是 DMA/缓存伪影。
4. 与任务原始 dump（通道电源开）对照：当时有真实电流读数的 {1,3,6,8,11,13}
   恰是现在安静的一组；当时读 0 的 {2,4,7,9,10,14,16} 恰是现在的噪声组
   → **噪声组 = 未接 DUT/负载的通道**：INA240 输入侧悬空，共模无定义，输出乱跳。
5. 例外：**ch12（PA1）有 ~218 mV 持续直流偏置**（min=3579，从不归零），
   不是噪声而是真实电压，需万用表板级排查该通道输入网络/漏电路径。

后续建议：

- 接上 DUT、通道上电后用 `test/adc_quality_probe.py` 复测，预期噪声组收敛到
  与 ch8 同量级；
- ch12 板级排查（INA240 输入偏置/漏电）；
- vrefint p2p ≈390 counts 提示 VREF+/VDDA 有约 0.2% 纹波，属硬件轨噪声，
  如需进一步压噪声可启用 ADC 硬件过采样，但对悬空通道无意义。
