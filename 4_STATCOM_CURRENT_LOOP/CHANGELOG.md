# Changelog - 4_STATCOM_CURRENT_LOOP

All notable changes to the 4_STATCOM_CURRENT_LOOP DSP project will be documented in this file.

## [0.1.5] - 2026-08-19

### Added
- 建立独立4号开发工程目录，配套上位机与CPLD基线 `0x0105`。
- 新增原始码保护快速路径：20 kHz ADC中断直接完成电网正半波峰值过压、交流瞬时过流保护判定（3点确认）。
- 新增原始码保持寄存器 `1000` (Vdc)、`1001` (电网电压)、`1002` (交流电流偏差) 读写支持。
- 新增 `0007` 交流电流零点校准原始码上报，由上位机执行工程量换算。

### Changed
- ADC采样保持20 kHz，SOGI-PLL调整为5:1严格均匀抽取的 4 kHz 队列处理，彻底消除采样时基抖动。
- `sci_take_frame()` 采用无全局关中断的环形缓冲区取帧机制，消除帧处理对 20 kHz ADC 中断的阻塞。
- 主循环实时处理模块（`app/`）保留调试符号并启用 `--opt_level=2` 优化，确保 4 kHz PLL 队列及时排空。
- STOP 状态清故障逻辑优化：重新武装“曾锁定后失锁”监测，允许清除历史失锁，START 仍强制要求 PLL 锁定且有效。

### Fixed
- 修复因全量 20 kHz 浮点计算与关中断导致的 `adc_queue_overflow` 及偶发 PLL 失锁问题。

### Security
- `STATCOM_POWER_OUTPUT_ENABLED=0` 保持硬封锁，禁止实际功率 PWM 输出。
