# 3_STATCOM_PI_PR

TMS320F28062双SCI通信与半波SOGI-PLL调试工程。在2号工程基础上增量开发，保留三路ADC、零漂校准和`ramfuncs`启动修复。

## 当前功能

- 90 MHz系统时钟；ePWM1 SOCA以20 kHz触发ADC，`TBPRD=4499`。
- SOC0/1/2依次采集ADCINA0电网正半波、ADCINA1直流电流、ADCINA2交流电流；EOC2触发ADCINT1。
- ADCINA0采用400点（一周期）滑动平均去直流并乘2，在每次ADC中断内运行SOGI-PLL。
- SCI-A（GPIO12 TX、GPIO7 RX）作为地址1 CPLD的Modbus RTU主站。
- SCI-B（GPIO22 TX、GPIO23 RX）作为地址2 DSP的Modbus RTU从站。
- 两条链路均为115200 bit/s、8N1，CRC16/A001且CRC低字节先发送。
- SCI发送采用逐字节FIFO装载并等待发送完成，避免连续写入导致回复帧头或数据字节丢失。
- `STATCOM_POWER_OUTPUT_ENABLED=0`；START/STOP只记录、转发和回读，不能产生PWM。

完整寄存器表、Modbus Poll设置和调试顺序见[docs/MODBUS_RTU_REGISTER_MAP.md](docs/MODBUS_RTU_REGISTER_MAP.md)。

开发专用Windows上位机时，使用[docs/STATCOM上位机设计与通信协议规范_V1.md](docs/STATCOM上位机设计与通信协议规范_V1.md)作为需求和验收依据。

## 构建与上板边界

CCS命令行Debug构建已经通过。`adc_isr_max_cycles`用于上板测量ADC ISR最长执行时间；20 kHz下验收目标为小于1350个90 MHz周期（15 us）。PLL输入必须确认是电网正半波而不是平滑直流母线，否则停止锁相测试。

当前工程只用于低压通信、采样和锁相调试。电流环、电压环、PWM产生和参数掉电保存均未实现。

## 当前验证状态

- CCS Debug配置已完整编译、链接通过。
- SCI-B在115200 bit/s、8N1下已通过串口助手实测：FC03读保持寄存器、FC04读取1个及23个输入寄存器、FC06写STOP/START均能返回完整且CRC正确的响应帧。
- SCI-B非法命令值返回异常`03`，非法寄存器地址返回异常`02`。
- SCI-A已在串口助手侧观察到连续、完整的CPLD命令帧，例如START命令`01 06 01 00 00 01 49 F6`；CPLD接入后的双向应答、超时及离线判断仍需联调。
- 上位机Python源码和协议单元测试位于`docs/statcom_host`；`build`、`dist`及Python缓存属于生成物，不纳入版本管理。
