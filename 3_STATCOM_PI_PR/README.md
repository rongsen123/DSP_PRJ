# 3_STATCOM_PI_PR

TMS320F28062双SCI通信与半波SOGI-PLL调试工程。在2号工程基础上增量开发，保留两路ADC、交流电流零漂校准和`ramfuncs`启动修复。

## 当前功能

- 90 MHz系统时钟；ePWM1 SOCA以20 kHz触发ADC，`TBPRD=4499`。
- SOC0采集ADCINA0电网正半波，SOC1采集ADCINA2交流电流；EOC1触发ADCINT1，ADCINA1直流电流通道已停用。
- ADC硬件仍以20 kHz采样；ADC中断每5次固定保留1组，形成严格均匀的4 kHz队列。主循环批量排空队列并执行交流电流零漂、模拟量换算、80点滑动平均和SOGI-PLL。
- `adc_processed_count`和`adc_pll_sample_count`用于确认4 kHz处理节拍；正常全速运行时两者约等于`adc_sample_count/5`，`adc_queue_overflow_count`应保持0。
- SCI-A（GPIO12 TX、GPIO7 RX）作为地址1 CPLD的Modbus RTU主站。
- SCI-B（GPIO22 TX、GPIO23 RX）作为地址2 DSP的Modbus RTU从站。
- 两条链路均为115200 bit/s、8N1，CRC16/A001且CRC低字节先发送。
- SCI发送采用逐字节FIFO装载并等待发送完成，避免连续写入导致回复帧头或数据字节丢失。
- SCI-A/SCI-B接收会检测硬件状态与FIFO溢出，错误时复位接收器并丢弃残帧；协议V0x0103将CPLD UART、CRC和不完整帧错误分开回读。
- 上位机可向DSP保持寄存器`0101`写入`0xA55A`请求清除CPLD锁存故障；DSP确保CPLD处于STOP回显后再转发，仍存在的故障源不会被清除。
- `STATCOM_POWER_OUTPUT_ENABLED=0`；START/STOP只记录、转发和回读，不能产生PWM。

完整寄存器表、Modbus Poll设置和调试顺序见[docs/MODBUS_RTU_REGISTER_MAP.md](docs/MODBUS_RTU_REGISTER_MAP.md)。

开发专用Windows上位机时，以[docs/statcom_host/上位机V0103修改交底书.md](docs/statcom_host/上位机V0103修改交底书.md)作为当前修改和验收依据；旧V1规范仅作历史参考。

## 构建与上板边界

CCS命令行Debug构建已经通过。`adc_isr_max_cycles`用于上板测量ADC ISR最长执行时间；20 kHz下验收目标为小于1350个90 MHz周期（15 us）。PLL输入必须确认是电网正半波而不是平滑直流母线，否则停止锁相测试。

当前工程只用于低压通信、采样和锁相调试。电流环、电压环、PWM产生和参数掉电保存均未实现。

## 当前验证状态

- CCS Debug配置已完整编译、链接通过。
- SCI-B在115200 bit/s、8N1下已通过串口助手实测：FC03读保持寄存器、FC04读取1个及23个输入寄存器、FC06写STOP/START均能返回完整且CRC正确的响应帧。
- SCI-B非法命令值返回异常`03`，非法寄存器地址返回异常`02`。
- DSP、CPLD与上位机V0x0103已完成配套上板联调；DSP-CPLD链路连续运行约47分钟、累计超过10万帧，DSP侧CRC、格式、超时、接收溢出均为0，CPLD侧UART、CRC和不完整帧计数均为0。
- 上位机Python源码和协议测试位于`docs/statcom_host`，直接运行`tests/test_protocol.py`验证通过；`build`、`dist`、EXE、数据日志及Python缓存属于生成物，不纳入版本管理。

## 版本记录

### 2026-08-17：两路ADC与4 kHz固定时基PLL

- ADC由三路精简为两路：ADCINA0电网正半波和ADCINA2交流电流。
- ePWM/ADC转换频率保持20 kHz，ADC中断端采用5:1确定性抽取，队列和SOGI-PLL运行频率调整为4 kHz。
- PLL直流均值窗口改为80点，锁定、解锁、信号丢失和数字滤波参数按4 kHz重新换算，50/150/250 Hz陷波器系数同步重算。
- 前台每次最多批量处理128个保留样本，并增加ADC已处理数和PLL运行数诊断计数，解决20 kHz全量前台浮点处理造成的队列溢出和PLL采样时基抖动。
- CCS Debug完整编译链接通过；4 kHz锁相参考向量的标称频率、频率范围、低压输入、畸变噪声、信号丢失/削顶和坐标帧共6项测试通过。
- Modbus寄存器、双SCI通信和功率输出封锁策略不变，协议版本继续使用`0x0103`。
