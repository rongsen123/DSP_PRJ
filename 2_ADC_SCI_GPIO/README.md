# 2_ADC_SCI_GPIO

## 工程功能

本工程运行于 TMS320F28062，在 `1_SCI_GPIO` 的 SCI-A 中断通信基础上，增加由 ePWM1 SOCA 硬件触发的三通道 ADC 周期采集。工程用于验证板卡直流母线电压、直流电流和交流电流采样链路，为后续电压环、电流环控制开发准备可靠的采样数据。

主要功能：

- SCI-A RX/TX FIFO 中断通信、协议帧解析校验及合法请求回送。
- ePWM1 SOCA 以 10 kHz 周期直接触发 ADC，不启用 ePWM CPU 中断。
- SOC0、SOC1、SOC2 按顺序采集三路模拟信号。
- SOC2 转换完成产生 EOC2，并由 EOC2 触发 ADCINT1。
- ADCINT1 服务函数读取三路原始结果、清除 ADC 标志并应答 PIE。
- 主循环完成电流零漂校准、ADC 引脚电压换算、交流电流低通滤波和真有效值计算。
- 启动时将 `ramfuncs` 从 Flash 装载地址复制到 RAM 运行地址，确保 `DELAY_US()` 正常执行。

## ADC通道与时序

| SOC | ADC通道 | 原理图网络 | 采集量 | 结果寄存器 |
| --- | --- | --- | --- | --- |
| SOC0 | ADCINA0 | VDC1 | 直流母线电压 | ADCRESULT0 |
| SOC1 | ADCINA1 | IDC1 | 直流电流 | ADCRESULT1 |
| SOC2 | ADCINA2 | IAC1 | 交流电流 | ADCRESULT2 |

采样链路：

```text
ePWM1 SOCA (10 kHz)
    -> SOC0 / ADCINA0
    -> SOC1 / ADCINA1
    -> SOC2 / ADCINA2
    -> EOC2
    -> ADCINT1
```

时钟配置：

| 项目 | 配置 |
| --- | --- |
| SYSCLKOUT | 90 MHz |
| ePWM1 TBCLK | 90 MHz |
| ADC时钟 | 45 MHz，SYSCLKOUT/2 |
| ePWM计数模式 | 向上计数 |
| TBPRD | 8999 |
| SOCA事件 | TBCTR=0，每次事件触发 |
| 单通道采样率 | 10 kSPS |
| ADCINT1频率 | 10 kHz |
| ACQPS | 14，即15个ADC时钟采样窗口 |

## 数据换算与零漂校准

板卡使用外部 REF2030 3.0 V 基准：

```text
ADC引脚电压 = raw * 3.0 / 4096
母线电压    = vdc_raw * 0.2741878 V
直流电流    = (idc_raw - idc_offset) * 0.09887695 A
交流电流    = (iac_raw - iac_offset) * 0.09887695 A
```

启动后自动使用 1024 个采样点计算 IDC、IAC 的平均零点。10 kHz 下校准窗口约为 102.4 ms，校准期间必须保证两个电流通道实际电流为零。VDC 是单极性通道，不执行自动零点扣除，避免带电启动时把真实母线电压误认为偏移。

浮点换算和零漂累加不在 ADC ISR 中执行。ADC ISR 保持最小路径，主循环调用 `adc_process()` 处理已经发布的一组三通道数据，避免高优先级中断占用过长影响 SCI。

## 交流电流滤波与有效值

`adc_value.iac_a` 保留零点扣除后的瞬时交流电流。程序对它增加一阶低通滤波：采样率 10 kHz、截止频率 500 Hz，结果为 `adc_value.iac_filtered_a`。该截止频率对 50 Hz、60 Hz 基波的幅值衰减均小于 1%，同时能够降低高频开关纹波和 ADC 抖动。

有效值使用 2000 个滤波后样本计算，窗口为 200 ms（50 Hz 的 10 周期、60 Hz 的 12 周期）。计算公式为 `sqrt(mean(i*i) - mean(i)^2)`，因此残余直流偏置不会被计入交流有效值。第一窗完成后 `adc_iac_status.rms_valid` 置 1，`adc_value.iac_rms_a` 每 200 ms 更新一次，可直接与交流电流源给定的 RMS 值比较。

若 `adc_iac_status.dropped_count` 持续增加，说明主循环来不及处理 10 kHz 数据，此时有效值窗口的时间尺度不再准确，应先排查后台阻塞或降低采样率。

## CCS Debug观察变量

程序运行后，可在 Expressions 中观察：

```c
system_startup_stage
main_loop_count
adc_sample_count
adc_overflow_count
adc_sample.vdc_raw
adc_sample.idc_raw
adc_sample.iac_raw
adc_value.vdc_pin_v
adc_value.idc_pin_v
adc_value.iac_pin_v
adc_value.vdc_bus_v
adc_value.idc_a
adc_value.iac_a
adc_value.iac_filtered_a
adc_value.iac_rms_a
adc_iac_status.rms_valid
adc_iac_status.rms_sample_count
adc_iac_status.rms_update_count
adc_iac_status.processed_count
adc_iac_status.dropped_count
adc_zero_calibration.state
adc_zero_calibration.sample_count
adc_zero_calibration.idc_offset_count
adc_zero_calibration.iac_offset_count
```

正常运行时：

- `system_startup_stage == 6`；
- `main_loop_count` 持续增加；
- `adc_sample_count` 约以 10000 次/秒增加；
- `adc_zero_calibration.state` 最终变为 `2`；
- `adc_overflow_count` 应保持为 0 或极少变化。
- `adc_iac_status.rms_valid == 1` 后，`adc_value.iac_rms_a` 才是可用的交流有效值；
- `adc_iac_status.dropped_count` 应保持为 0。

## SCI配置

| 项目 | 配置 |
| --- | --- |
| 波特率 | 230400 bit/s |
| 数据格式 | 8数据位、无校验、1停止位 |
| SCIRXDA | GPIO7 |
| SCITXDA | GPIO12 |
| 状态LED | GPIO10、GPIO55 |

协议帧：

```text
帧头1 | 帧头2 | 数据长度 | 数据区 | 校验
 AA   |  55   |   LEN    | DATA   | XOR
```

## 软件结构

- `main.c`：系统初始化、`ramfuncs`复制、SCI/ADC启动和后台数据处理。
- `app/adc.c`、`app/adc.h`：ADC SOC、ePWM1 SOCA、ADCINT1、零漂及模拟量换算。
- `app/sci.c`、`app/sci.h`：SCI中断、协议解析和帧发送。
- `app/led.c`、`app/led.h`：状态LED GPIO。
- `source/`、`headers/`、`include/`：F2806x设备支持代码及寄存器定义。
- `cmd/`：F28062 Flash/RAM和外设寄存器链接配置。

## 编译与验证

1. 使用 CCS 12.3.0 导入本目录中的既有 CCS 工程。
2. 使用 TI C2000 22.6.0.LTS 编译器，目标器件选择 TMS320F28062。
3. 选择 Debug 配置，执行 Clean Project 和 Build Project。
4. 使用 XDS100 USB 连接目标板，加载 `Debug/2_ADC_SCI_GPIO.out`。
5. CPU Reset、Restart、Resume 后，通过上述 Expressions 变量检查采样、零漂和模拟量结果。

源码已经使用 TI C2000 22.6.0.LTS 完整编译、链接通过。`Debug/`、`.out`、`.map` 等生成文件不纳入版本管理。硬件上的最终采样比例和零点仍需使用标准电压、电流源复核标定。
