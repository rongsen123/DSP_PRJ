"""寄存器模型：将原始寄存器值解析为物理量、状态位与枚举文本。

数据来源依据 ``STATCOM上位机寄存器映射_V1.json``。
"""

from __future__ import annotations

# 输入寄存器地址（FC04 从 0x0000 起，共 23 个）
INPUT_QUANTITY = 23

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
    10: "uart_frame_error",
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


def parse_input_registers(values: list[int] | tuple[int, ...]) -> dict:
    """解析 23 个输入寄存器的原始值，返回结构化字典。"""
    if len(values) < INPUT_QUANTITY:
        raise ValueError(f"输入寄存器数量不足：需 {INPUT_QUANTITY}，实得 {len(values)}")

    v = values
    data: dict = {}

    # 协议与计时
    data["protocol_version"] = v[0]
    data["uptime_ms"] = uint32(v[1], v[2])
    data["adc_sample_count"] = uint32(v[3], v[4])

    # 模拟量
    data["grid_halfwave_raw"] = v[5]
    data["grid_voltage"] = round(v[5] * 0.2741878, 2)  # 暂定增益，待标定
    data["iac_raw"] = v[6]
    data["iac"] = round(to_signed(v[7]) * 0.01, 2)  # 单位 A
    data["cpld_vdc_raw"] = v[8]
    data["cpld_vdc_average"] = v[9]
    data["temperature_count"] = v[10]  # 脉冲/100ms，待标定

    # 状态位
    data["cpld_status"] = decode_bitfield(v[11], CPLD_STATUS_BITS)
    cpld_fault_raw = uint32(v[12], v[13])
    data["cpld_fault_raw"] = cpld_fault_raw
    data["cpld_fault"] = decode_bitfield(cpld_fault_raw, CPLD_FAULT_BITS)
    data["cpld_link"] = decode_bitfield(v[14], CPLD_LINK_BITS)

    data["pll_locked"] = bool(v[15])
    data["pll_frequency"] = round(v[16] * 0.01, 2)  # 单位 Hz
    data["pll_signal_valid"] = bool(v[17])

    data["statcom_state_raw"] = v[18]
    data["statcom_state"] = STATCOM_STATES.get(v[18], f"0x{v[18]:04X}")
    data["adc_zero_state_raw"] = v[19]
    data["adc_zero_state"] = ADC_ZERO_STATES.get(v[19], f"0x{v[19]:04X}")

    data["dsp_cpld_error_count"] = v[20]
    data["pc_dsp_error_count"] = v[21]
    data["cpld_command_echo_raw"] = v[22]
    data["cpld_command_echo"] = COMMANDS.get(v[22], f"0x{v[22]:04X}")

    # 汇总故障
    data["fault_active"] = data["cpld_status"].get("fault_any", False) or any(
        name not in ("bypass_status",) and value
        for name, value in data["cpld_fault"].items()
    )
    return data


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
        "uart_frame_error": "UART 帧错误",
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