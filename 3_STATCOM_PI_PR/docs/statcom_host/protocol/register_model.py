"""寄存器模型：将原始寄存器值解析为物理量、状态位与枚举文本。

数据来源依据《单相STATCOM上位机 V0x0103 修改交底书》与最新电路理论比例。
"""

from __future__ import annotations

# 输入寄存器数量
INPUT_QUANTITY_V103 = 29
INPUT_QUANTITY_V101 = 23
INPUT_QUANTITY = INPUT_QUANTITY_V103
INPUT_QUANTITY_MIN = INPUT_QUANTITY_V101

# 协议版本常量
MIN_MAINTENANCE_PROTOCOL_VERSION = 0x0101
MIN_RESET_PROTOCOL_VERSION = 0x0102
MIN_ADC_RECALIBRATION_PROTOCOL_VERSION = 0x0102
PROTOCOL_VERSION_V103 = 0x0103

# 保持寄存器地址与操作密钥
ADDR_COMMAND = 0x0100
CMD_STOP = 0x0000
CMD_START = 0x0001

ADDR_CLEAR_CPLD_FAULT = 0x0101
KEY_CLEAR_CPLD_FAULT = 0xA55A

ADDR_RESET_CPLD = 0x0102
KEY_RESET_CPLD = 0xC33C

ADDR_RESET_DSP = 0x0103
KEY_RESET_DSP = 0xD55D

ADDR_ADC_RECALIBRATE = 0x0104
KEY_ADC_RECALIBRATE = 0xCA1B

# 物理量工程转换默认初值 (按新电路理论比例: K = 5*4604.6 / (4095*8.2*2.4) ≈ 0.2857 V/count)
DEFAULT_VDC_GAIN = 0.28568177
DEFAULT_VDC_ZERO = 0.0


def two_point_calibrate(c1: float, v1: float, c2: float, v2: float) -> tuple[float, float]:
    """两点标定计算增益与零点码：V = (C - zero_code) * gain。"""
    if abs(c2 - c1) < 1e-6:
        raise ValueError("标定两点 ADC 码值不能相同")
    gain = (v2 - v1) / (c2 - c1)
    zero_code = c1 - (v1 / gain) if abs(gain) > 1e-6 else 0.0
    return gain, zero_code


def calc_vdc_voltage(raw_count: int | float, gain: float = DEFAULT_VDC_GAIN, zero_code: float = DEFAULT_VDC_ZERO) -> float:
    """按标定参数将 CPLD Vdc ADC 码换算为直流电压 (V)：Vdc = max(0, (raw_count - zero_code) * gain)。"""
    v = (float(raw_count) - float(zero_code)) * float(gain)
    return max(0.0, v)


def calc_temperature_frequency(count: int | float) -> float:
    """将 100ms 窗口内的脉冲计数换算为频率 (Hz)。"""
    return float(count) * 10.0


def calc_delta_u16(current: int, previous: int) -> int:
    """计算 16 位无符号累加计数器的增量，正确处理回绕。"""
    return (current - previous) & 0xFFFF


def supports_remote_maintenance(protocol_version: int) -> bool:
    """判断固件版本是否支持远程清除 CPLD 锁存故障。"""
    return (protocol_version & 0xFFFF) >= MIN_MAINTENANCE_PROTOCOL_VERSION


def supports_adc_recalibration(protocol_version: int) -> bool:
    """判断固件版本是否支持 STOP 联锁的 ADC 重新零漂校准。"""
    return (protocol_version & 0xFFFF) >= MIN_ADC_RECALIBRATION_PROTOCOL_VERSION


def supports_remote_reset(protocol_version: int) -> bool:
    """判断固件版本是否支持 0x0102/0x0103 受控复位。"""
    return (protocol_version & 0xFFFF) >= MIN_RESET_PROTOCOL_VERSION


def supports_v103_extended_registers(protocol_version: int) -> bool:
    """判断固件版本是否支持 29 个输入寄存器扩展诊断。"""
    return (protocol_version & 0xFFFF) >= PROTOCOL_VERSION_V103


# 枚举表
COMMANDS = {0: "STOP", 1: "START"}
ADC_ZERO_STATES = {0: "IDLE", 1: "RUNNING", 2: "DONE"}
STATCOM_STATES = {
    0: "SAFE_BOOT",
    1: "ADC_VERIFY",
    2: "LINK_VERIFY",
    3: "PLL_MONITOR",
    4: "CONTROL_SHADOW",
    5: "CURRENT_LOOP",
    6: "VOLTAGE_LOOP",
    7: "RUN",
    8: "FAULT",
}

# 位域定义
CPLD_STATUS_BITS = {
    0: "link_online",
    1: "adc_valid",
    2: "temperature_valid",
    3: "fault_any",
    4: "pwm_healthy",
}

CPLD_LINK_BITS = {
    0: "online",
    1: "timeout_offline",
    2: "crc_error_seen",
    3: "exception_seen",
}

CPLD_FAULT_BITS = {
    0: "bypass_status",
    1: "peer_module_fault",
    4: "dc_overvoltage_fault",
    5: "drive_fault_1",
    6: "drive_fault_2",
    7: "software_vdc_overvoltage",
    8: "temperature_over",
    10: "cpld_rx_error_latch",
    11: "temperature_sensor_fault",
    12: "precharge_under",
    13: "precharge_over",
    14: "precharge_timeout",
    15: "adc_stale",
    18: "pwm_monitor_fault",
    19: "dsp_cpld_link_timeout",
    22: "config_invalid",
}


def to_signed(v16: int) -> int:
    """将 16 位无符号寄存器值转换为有符号整数。"""
    v = v16 & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def uint32(high: int, low: int) -> int:
    """高位字在前组合 32 位无符号整数。"""
    return ((high & 0xFFFF) << 16) | (low & 0xFFFF)


def decode_bitfield(value: int, bit_map: dict[int, str]) -> dict[str, bool]:
    """按位域映射表解码，返回 {名称: 是否置位}。"""
    out = {name: False for name in bit_map.values()}
    for bit, name in bit_map.items():
        out[name] = bool(value & (1 << bit))
    return out


# 29 个输入寄存器的标准定义元数据表
REGISTER_DEFINITIONS: list[dict] = [
    {
        "addr": 0x0000,
        "name": "DSP 协议版本号",
        "category": "系统信息",
        "unit": "hex",
        "desc": "上位机与 DSP 通信协议版本 (如 0x0103)",
    },
    {
        "addr": 0x0001,
        "name": "DSP 运行时间 (高16位)",
        "category": "系统信息",
        "unit": "ms",
        "desc": "系统上电/复位后累计运行时间的高字 (单位: 毫秒)",
    },
    {
        "addr": 0x0002,
        "name": "DSP 运行时间 (低16位)",
        "category": "系统信息",
        "unit": "ms",
        "desc": "系统累计运行时间的低字，组合构成 32 位时间戳",
    },
    {
        "addr": 0x0003,
        "name": "ADC 中断采样计数 (高16位)",
        "category": "采样统计",
        "unit": "count",
        "desc": "EPWM 触发 ADC 中断的总累加采样次数高字",
    },
    {
        "addr": 0x0004,
        "name": "ADC 中断采样计数 (低16位)",
        "category": "采样统计",
        "unit": "count",
        "desc": "EPWM 触发 ADC 中断的总累加采样次数低字",
    },
    {
        "addr": 0x0005,
        "name": "电网电压正半波 (ADCINA0)",
        "category": "电网遥测",
        "unit": "count / V",
        "desc": "电网电压正半波整流瞬时 ADC 码 (0~4095)",
    },
    {
        "addr": 0x0006,
        "name": "交流电流瞬时码 (ADCINA2)",
        "category": "电流遥测",
        "unit": "count",
        "desc": "STATCOM 交流侧电流传感器原始未校零采样码",
    },
    {
        "addr": 0x0007,
        "name": "交流电流实际值 Iac",
        "category": "电流遥测",
        "unit": "0.01 A",
        "desc": "经零漂校准与工程定标后的有符号电流值 (LSB=0.01A)",
    },
    {
        "addr": 0x0008,
        "name": "CPLD Vdc 瞬时码 (ADS7818)",
        "category": "直流母线",
        "unit": "count",
        "desc": "CPLD 端 ADS7818 采集的母线电压瞬时 12 位原始码",
    },
    {
        "addr": 0x0009,
        "name": "CPLD Vdc 256点平均码",
        "category": "直流母线",
        "unit": "count / V",
        "desc": "CPLD 硬件 256 点滑动滤波平均码 (用于计算直流电压)",
    },
    {
        "addr": 0x000A,
        "name": "IGBT 温度脉冲计数",
        "category": "温度监控",
        "unit": "count/100ms",
        "desc": "100ms 窗口内脉冲数，对应频率 f = count * 10 Hz",
    },
    {
        "addr": 0x000B,
        "name": "CPLD 硬件状态位域",
        "category": "CPLD状态",
        "unit": "bitfield",
        "desc": "Bit0链路在线、Bit1 ADC有效、Bit2温度有效、Bit3总故障、Bit4 PWM健康",
    },
    {
        "addr": 0x000C,
        "name": "CPLD 32位锁存故障 (高16位)",
        "category": "故障报警",
        "unit": "bitfield",
        "desc": "Bit16~Bit31 (含过压、欠压、超时、PWM封锁等硬件保护)",
    },
    {
        "addr": 0x000D,
        "name": "CPLD 32位锁存故障 (低16位)",
        "category": "故障报警",
        "unit": "bitfield",
        "desc": "Bit0~Bit15 (含旁路、驱动故障、通信接收错误Bit10等)",
    },
    {
        "addr": 0x000E,
        "name": "DSP-CPLD 链路状态位域",
        "category": "链路诊断",
        "unit": "bitfield",
        "desc": "Bit0当前在线、Bit1 500ms超时离线、Bit2 CRC历史错误、Bit3 异常响应",
    },
    {
        "addr": 0x000F,
        "name": "SOGI-PLL 锁相环锁定标志",
        "category": "锁相环",
        "unit": "bool",
        "desc": "0: 未锁定, 1: 已成功锁定电网基波相位与频率",
    },
    {
        "addr": 0x0010,
        "name": "SOGI-PLL 电网频率",
        "category": "锁相环",
        "unit": "0.01 Hz",
        "desc": "锁相环跟踪的电网实时基波频率 (如 5000 代表 50.00 Hz)",
    },
    {
        "addr": 0x0011,
        "name": "SOGI-PLL 输入信号有效标志",
        "category": "锁相环",
        "unit": "bool",
        "desc": "0: 电网正半波异常/无信号, 1: 信号幅值有效",
    },
    {
        "addr": 0x0012,
        "name": "STATCOM 主控运行状态",
        "category": "主控制",
        "unit": "enum",
        "desc": "0:SAFE_BOOT, 1:ADC_VERIFY, 3:PLL_MONITOR, 7:RUN, 8:FAULT 等",
    },
    {
        "addr": 0x0013,
        "name": "ADC 电流零漂自校准状态",
        "category": "自校准",
        "unit": "enum",
        "desc": "0: IDLE(未校准), 1: RUNNING(1024点采样中), 2: DONE(校准完成)",
    },
    {
        "addr": 0x0014,
        "name": "DSP-CPLD 通信综合错误计数",
        "category": "通信诊断",
        "unit": "次",
        "desc": "DSP 观察到的 SCI-A 错误总数 (含 CRC/格式/超时/异常)",
    },
    {
        "addr": 0x0015,
        "name": "PC-DSP 通信累计错误计数",
        "category": "通信诊断",
        "unit": "次",
        "desc": "DSP 接收上位机 SCI-B 帧时捕获的 CRC/格式错误累计",
    },
    {
        "addr": 0x0016,
        "name": "CPLD 命令回显状态",
        "category": "主控制",
        "unit": "enum",
        "desc": "0: STOP (安全停机), 1: START (调试运行回显)",
    },
    {
        "addr": 0x0017,
        "name": "CPLD UART 停止位错误计数",
        "category": "CPLD接收",
        "unit": "次",
        "desc": "CPLD 接收 DSP 串口帧时因停止位未拉高产生的 Framing Error",
    },
    {
        "addr": 0x0018,
        "name": "CPLD Modbus CRC 错误计数",
        "category": "CPLD接收",
        "unit": "次",
        "desc": "CPLD 收到请求帧但 CRC16 校验不匹配的累计次数",
    },
    {
        "addr": 0x0019,
        "name": "CPLD t3.5 残帧超时计数",
        "category": "CPLD接收",
        "unit": "次",
        "desc": "CPLD 接收字节未达一帧且间隔超过 1.75ms 的丢弃残帧数",
    },
    {
        "addr": 0x001A,
        "name": "DSP SCI-A 硬件格式错误",
        "category": "DSP接收",
        "unit": "次",
        "desc": "DSP 硬件捕获的校验/溢出/帧格式错误 (PE/OE/FE/BRKDT/RXERROR)",
    },
    {
        "addr": 0x001B,
        "name": "DSP SCI-A 接收溢出计数",
        "category": "DSP接收",
        "unit": "次",
        "desc": "DSP 硬件 FIFO 或内部软件环形缓冲区满溢出的丢包计数",
    },
    {
        "addr": 0x001C,
        "name": "DSP 等待 CPLD 回复超时计数",
        "category": "DSP接收",
        "unit": "次",
        "desc": "DSP 发出读/写请求后 20ms 内未收到 CPLD 响应的超时重试数",
    },
]


def parse_input_registers(
    values: list[int] | tuple[int, ...],
    vdc_gain: float = DEFAULT_VDC_GAIN,
    vdc_zero: float = DEFAULT_VDC_ZERO,
) -> dict:
    """解析输入寄存器的原始值，返回结构化字典（兼容 23 与 29 寄存器）。"""
    if len(values) < INPUT_QUANTITY_MIN:
        raise ValueError(f"输入寄存器数量不足：最少需 {INPUT_QUANTITY_MIN}，实得 {len(values)}")

    v = values
    data: dict = {}

    # 协议与计时 (0x0000 ~ 0x0004)
    data["protocol_version"] = v[0]
    data["uptime_ms"] = uint32(v[1], v[2])
    data["adc_sample_count"] = uint32(v[3], v[4])

    # 模拟量 (0x0005 ~ 0x000A)
    data["grid_halfwave_raw"] = v[5]
    data["grid_voltage"] = round(v[5] * 0.2741878, 2)  # 暂定增益，待标定
    data["iac_raw"] = v[6]
    data["iac"] = round(to_signed(v[7]) * 0.01, 2)  # 单位 A
    data["cpld_vdc_raw"] = v[8]
    data["cpld_vdc_average"] = v[9]
    data["cpld_vdc_v"] = round(calc_vdc_voltage(v[9], vdc_gain, vdc_zero), 1)
    data["temperature_count"] = v[10]
    data["temperature_frequency_hz"] = round(calc_temperature_frequency(v[10]), 1)

    # 状态位 (0x000B ~ 0x000E)
    data["cpld_status"] = decode_bitfield(v[11], CPLD_STATUS_BITS)
    cpld_fault_raw = uint32(v[12], v[13])
    data["cpld_fault_raw"] = cpld_fault_raw
    data["cpld_fault"] = decode_bitfield(cpld_fault_raw, CPLD_FAULT_BITS)
    data["cpld_link"] = decode_bitfield(v[14], CPLD_LINK_BITS)

    # 锁相环状态 (0x000F ~ 0x0011)
    data["pll_locked"] = bool(v[15])
    data["pll_frequency"] = round(v[16] * 0.01, 2)  # 单位 Hz
    data["pll_signal_valid"] = bool(v[17])

    # 系统状态 (0x0012 ~ 0x0016)
    data["statcom_state_raw"] = v[18]
    data["statcom_state"] = STATCOM_STATES.get(v[18], f"0x{v[18]:04X}")
    data["adc_zero_state_raw"] = v[19]
    data["adc_zero_state"] = ADC_ZERO_STATES.get(v[19], f"0x{v[19]:04X}")

    data["dsp_cpld_error_count"] = v[20]
    data["pc_dsp_error_count"] = v[21]
    data["cpld_command_echo_raw"] = v[22]
    data["cpld_command_echo"] = COMMANDS.get(v[22], f"0x{v[22]:04X}")

    # 扩展诊断寄存器 (0x0017 ~ 0x001C, V0x0103)
    if len(values) >= INPUT_QUANTITY_V103:
        data["cpld_uart_error_count"] = v[23]
        data["cpld_crc_error_count"] = v[24]
        data["cpld_incomplete_frame_count"] = v[25]
        data["dsp_scia_format_error_count"] = v[26]
        data["dsp_scia_overflow_count"] = v[27]
        data["dsp_cpld_timeout_count"] = v[28]
        data["has_extended_diag"] = True
    else:
        data["cpld_uart_error_count"] = 0
        data["cpld_crc_error_count"] = 0
        data["cpld_incomplete_frame_count"] = 0
        data["dsp_scia_format_error_count"] = 0
        data["dsp_scia_overflow_count"] = 0
        data["dsp_cpld_timeout_count"] = 0
        data["has_extended_diag"] = False

    # 汇总故障
    data["fault_active"] = data["cpld_status"].get("fault_any", False) or any(
        name not in ("bypass_status",) and value
        for name, value in data["cpld_fault"].items()
    )
    return data


def format_register_interpreted_value(
    addr: int,
    raw_val: int,
    all_raw: list[int] | None = None,
    vdc_gain: float = DEFAULT_VDC_GAIN,
    vdc_zero: float = DEFAULT_VDC_ZERO,
) -> str:
    """根据寄存器地址与原始值，生成便于直观理解的物理/状态解释文本。"""
    if addr == 0x0000:
        return f"V{raw_val >> 8}.{raw_val & 0xFF:02X} (0x{raw_val:04X})"
    if addr in (0x0001, 0x0002):
        if all_raw and len(all_raw) > 2:
            ms = uint32(all_raw[1], all_raw[2])
            return f"总时长: {ms/1000.0:.1f}s"
        return f"{raw_val} ms"
    if addr in (0x0003, 0x0004):
        if all_raw and len(all_raw) > 4:
            cnt = uint32(all_raw[3], all_raw[4])
            return f"总采样: {cnt} 次"
        return f"{raw_val}"
    if addr == 0x0005:
        v = raw_val * 0.2741878
        return f"{v:.1f} V (原始码: {raw_val})"
    if addr == 0x0006:
        return f"原始码: {raw_val}"
    if addr == 0x0007:
        iac = to_signed(raw_val) * 0.01
        return f"{iac:+.2f} A"
    if addr == 0x0008:
        v = calc_vdc_voltage(raw_val, vdc_gain, vdc_zero)
        return f"{v:.1f} V (瞬时码: {raw_val})"
    if addr == 0x0009:
        v = calc_vdc_voltage(raw_val, vdc_gain, vdc_zero)
        return f"{v:.1f} V (平均码: {raw_val})"
    if addr == 0x000A:
        f = calc_temperature_frequency(raw_val)
        return f"{f:.0f} Hz (待标定 ℃)"
    if addr == 0x000B:
        st = decode_bitfield(raw_val, CPLD_STATUS_BITS)
        online = "在线" if st["link_online"] else "离线"
        fault = "有故障" if st["fault_any"] else "无故障"
        return f"CPLD: {online} / {fault}"
    if addr in (0x000C, 0x000D):
        if all_raw and len(all_raw) > 13:
            f32 = uint32(all_raw[12], all_raw[13])
            if f32 == 0:
                return "✅ 0x00000000 (无故障)"
            return f"⚠️ 0x{f32:08X} (故障触发)"
        return f"0x{raw_val:04X}"
    if addr == 0x000E:
        lk = decode_bitfield(raw_val, CPLD_LINK_BITS)
        return "在线" if lk["online"] else ("超时离线" if lk["timeout_offline"] else "未知")
    if addr == 0x000F:
        return "✅ 已锁定" if raw_val == 1 else "❌ 未锁定"
    if addr == 0x0010:
        return f"{raw_val * 0.01:.2f} Hz"
    if addr == 0x0011:
        return "✅ 信号有效" if raw_val == 1 else "❌ 信号无效"
    if addr == 0x0012:
        return STATCOM_STATES.get(raw_val, f"0x{raw_val:04X}")
    if addr == 0x0013:
        return ADC_ZERO_STATES.get(raw_val, f"0x{raw_val:04X}")
    if addr == 0x0014:
        return f"{raw_val} 次 (DSP-CPLD综合错误)"
    if addr == 0x0015:
        return f"{raw_val} 次 (PC-DSP综合错误)"
    if addr == 0x0016:
        return COMMANDS.get(raw_val, f"0x{raw_val:04X}")
    if addr == 0x0017:
        return f"{raw_val} 次 (停止位错误)"
    if addr == 0x0018:
        return f"{raw_val} 次 (CRC错误)"
    if addr == 0x0019:
        return f"{raw_val} 次 (残帧超时)"
    if addr == 0x001A:
        return f"{raw_val} 次 (硬件格式错误)"
    if addr == 0x001B:
        return f"{raw_val} 次 (接收溢出)"
    if addr == 0x001C:
        return f"{raw_val} 次 (等待回复超时)"
    return f"0x{raw_val:04X}"


def active_fault_names(data: dict) -> list[str]:
    """返回当前置位故障的友好名称列表。"""
    btn = {
        "bypass_status": "旁路状态",
        "peer_module_fault": "相邻模块故障",
        "dc_overvoltage_fault": "直流过压故障",
        "drive_fault_1": "驱动故障 1",
        "drive_fault_2": "驱动故障 2",
        "software_vdc_overvoltage": "软件直流过压",
        "temperature_over": "温度超限",
        "cpld_rx_error_latch": "CPLD通信接收错误锁存（UART/CRC/残帧综合）",
        "uart_frame_error": "CPLD通信接收错误锁存（UART/CRC/残帧综合）",
        "temperature_sensor_fault": "温度传感器故障",
        "precharge_under": "预充电欠压",
        "precharge_over": "预充电过压",
        "precharge_timeout": "预充电超时",
        "adc_stale": "ADC 数据过期",
        "pwm_monitor_fault": "PWM 监测故障",
        "dsp_cpld_link_timeout": "DSP-CPLD 链路超时",
        "config_invalid": "配置无效",
    }
    return [btn.get(name, name) for name, value in data["cpld_fault"].items() if value]
