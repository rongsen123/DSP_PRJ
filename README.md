# SST DSP 工程集

本仓库用于统一管理 SST 项目相关的 TI C2000 DSP/CCS 工程。每个子目录对应一个可独立导入、编译和调试的工程，并在工程目录内提供单独的功能说明。

## 工程目录

| 工程 | 目标芯片 | 功能说明 |
| --- | --- | --- |
| [1_SCI_GPIO](1_SCI_GPIO/) | TMS320F28062 | 基于 SCI 收发中断实现光纤串口问答通信、协议帧解析校验及 LED 状态指示。 |
| [2_ADC_SCI_GPIO](2_ADC_SCI_GPIO/) | TMS320F28062 | 在 SCI 通信基础上增加 ePWM 触发的 10 kHz 三通道 ADC 中断采集、零漂校准及模拟量换算。 |
| [PLECS_prj](PLECS_prj/) | PLECS | 低压全桥整流仿真工程，用于后续控制策略与功率回路联合验证。 |

## 开发环境

- Code Composer Studio：12.3.0
- C2000 Code Generation Tools：22.6.0.LTS
- 目标器件：TMS320F28062
- 调试连接：TI XDS100 USB

## 版本管理约定

- 各工程源码、头文件、链接文件及必要 CCS 配置纳入版本管理。
- `Debug/`、`Release/`、`.out`、`.map` 等生成文件不提交。
- 新增工程时，应同步更新本文件的工程目录，并在工程目录内提供 `README.md`。
