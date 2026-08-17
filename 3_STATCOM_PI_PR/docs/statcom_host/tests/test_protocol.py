"""STATCOM 上位机 V0x0103 协议、换算与单元测试。"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from communication.crc16 import crc16_modbus, append_crc, verify_crc
from communication.modbus_rtu import (
    build_read_request,
    build_write_single_request,
    parse_response,
    parse_read_response,
    parse_write_single_response_exact,
)
from communication.serial_worker import (
    DspSimulator,
    SerialWorker,
    parse_input_block,
    parse_maintenance_back,
)
from protocol.register_model import (
    ADDR_COMMAND,
    ADDR_CLEAR_CPLD_FAULT,
    ADDR_RESET_CPLD,
    ADDR_RESET_DSP,
    ADDR_ADC_RECALIBRATE,
    CMD_STOP,
    CMD_START,
    KEY_CLEAR_CPLD_FAULT,
    KEY_RESET_CPLD,
    KEY_RESET_DSP,
    KEY_ADC_RECALIBRATE,
    PROTOCOL_VERSION_V103,
    INPUT_QUANTITY_V103,
    INPUT_QUANTITY_V101,
    calc_delta_u16,
    calc_vdc_voltage,
    calc_temperature_frequency,
    two_point_calibrate,
    parse_input_registers,
    supports_remote_maintenance,
    supports_remote_reset,
    supports_adc_recalibration,
    supports_v103_extended_registers,
    to_signed,
    uint32,
)


def test_crc16_and_known_frames():
    """验证规范与交底书中列出的所有已知帧 CRC。"""
    # 读 29 个寄存器 (V0103): 02 04 00 00 00 1D -> 63 字节响应
    req_fc04_29 = build_read_request(2, 0x04, 0x0000, 29)
    assert req_fc04_29 == bytes([0x02, 0x04, 0x00, 0x00, 0x00, 0x1D, 0x30, 0x30])
    assert verify_crc(req_fc04_29) is True

    # 读 23 个寄存器 (V0101-0102): 02 04 00 00 00 17 -> CRC = B0 37
    req_fc04_23 = build_read_request(2, 0x04, 0x0000, 23)
    assert req_fc04_23 == bytes([0x02, 0x04, 0x00, 0x00, 0x00, 0x17, 0xB0, 0x37])
    assert verify_crc(req_fc04_23) is True

    # 1. 上位机请求DSP清除CPLD故障: 02 06 01 01 A5 5A 22 AE
    frame_clear_fault = build_write_single_request(2, ADDR_CLEAR_CPLD_FAULT, KEY_CLEAR_CPLD_FAULT)
    assert frame_clear_fault == bytes([0x02, 0x06, 0x01, 0x01, 0xA5, 0x5A, 0x22, 0xAE])
    assert verify_crc(frame_clear_fault) is True

    # 2. 上位机请求复位CPLD: 02 06 01 02 C3 3C 79 24
    frame_reset_cpld = build_write_single_request(2, ADDR_RESET_CPLD, KEY_RESET_CPLD)
    assert frame_reset_cpld == bytes([0x02, 0x06, 0x01, 0x02, 0xC3, 0x3C, 0x79, 0x24])
    assert verify_crc(frame_reset_cpld) is True

    # 3. 上位机请求复位DSP: 02 06 01 03 D5 5D E7 6C
    frame_reset_dsp = build_write_single_request(2, ADDR_RESET_DSP, KEY_RESET_DSP)
    assert frame_reset_dsp == bytes([0x02, 0x06, 0x01, 0x03, 0xD5, 0x5D, 0xE7, 0x6C])
    assert verify_crc(frame_reset_dsp) is True

    # 4. 上位机请求ADC重新零漂校准: 02 06 01 04 CA 1B C4 D9
    frame_adc_recal = build_write_single_request(2, ADDR_ADC_RECALIBRATE, KEY_ADC_RECALIBRATE)
    assert verify_crc(frame_adc_recal) is True


def test_v103_29_registers_and_fallback():
    """测试 0x0103 协议 29 个寄存器读写与 0x0102 回退 23 寄存器兼容。"""
    sim_v103 = DspSimulator(slave=2, protocol_version=0x0103)
    sim_v103.cpld_uart_errors = 12
    sim_v103.cpld_crc_errors = 3
    sim_v103.cpld_incomplete_frame_errors = 1
    sim_v103.dsp_scia_format_errors = 4
    sim_v103.dsp_scia_overflow_errors = 2
    sim_v103.dsp_cpld_timeouts = 5

    # 1. 29 寄存器请求与 63 字节响应测试
    req29 = build_read_request(2, 0x04, 0x0000, 29)
    resp29 = sim_v103.handle_request(req29)
    assert resp29 is not None
    assert len(resp29) == 63  # 3 + 29*2 + 2 = 63 字节
    assert resp29[2] == 58    # 字节计数 0x3A = 58
    assert verify_crc(resp29) is True

    parsed, values = parse_input_block(resp29)
    assert len(values) == 29
    data = parse_input_registers(values)
    assert data["protocol_version"] == 0x0103
    assert data["has_extended_diag"] is True
    assert data["cpld_uart_error_count"] == 12
    assert data["cpld_crc_error_count"] == 3
    assert data["cpld_incomplete_frame_count"] == 1
    assert data["dsp_scia_format_error_count"] == 4
    assert data["dsp_scia_overflow_count"] == 2
    assert data["dsp_cpld_timeout_count"] == 5

    # 2. 旧版 0x0102 回退 23 寄存器请求与解析测试
    sim_v102 = DspSimulator(slave=2, protocol_version=0x0102)
    req23 = build_read_request(2, 0x04, 0x0000, 23)
    resp23 = sim_v102.handle_request(req23)
    assert resp23 is not None
    assert len(resp23) == 51  # 3 + 23*2 + 2 = 51 字节
    assert resp23[2] == 46    # 字节计数 0x2E = 46

    parsed_v102, values_v102 = parse_input_block(resp23)
    assert len(values_v102) == 23
    data_v102 = parse_input_registers(values_v102)
    assert data_v102["protocol_version"] == 0x0102
    assert data_v102["has_extended_diag"] is False


def test_engineering_units_and_calibration():
    """测试物理工程量换算与标定。"""
    # 1. 直流电压 Vdc 两点标定与换算
    # 测试两点标定: 点1 (ADC=100, V=36.0V), 点2 (ADC=2000, V=720.0V)
    gain, offset = two_point_calibrate(100.0, 36.0, 2000.0, 720.0)
    assert abs(gain - 0.36) < 1e-4
    assert abs(offset - 0.0) < 1e-4

    # 新电路理论换算: K ≈ 0.2857 (5*4604.6 / (4095*8.2*2.4))
    # 1) ADC=91 (对应约24V输入) -> 约 26.0 V (zero=0)
    v_91 = calc_vdc_voltage(91, gain=0.2857, zero_code=0.0)
    assert abs(v_91 - 26.0) < 0.1

    # 2) 空载零点测试: 未接输入时ADC=4，设置 zero_code=4.0 -> 减去后计算为 0.0V
    v_4_zero = calc_vdc_voltage(4, gain=0.2857, zero_code=4.0)
    assert abs(v_4_zero - 0.0) < 1e-4

    # 3) 减去零点后测量 24V (ADC=91): (91 - 4) * 0.2857 ≈ 24.85 V
    v_91_cal = calc_vdc_voltage(91, gain=0.2857, zero_code=4.0)
    assert abs(v_91_cal - 24.85) < 0.1

    # 默认比例换算: ADC=1276
    v_calc = calc_vdc_voltage(1276)
    assert abs(v_calc - 1276 * 0.28568177) < 1e-3

    # 2. 温度计数到频率换算 (418 count -> 4180 Hz)
    freq = calc_temperature_frequency(418)
    assert abs(freq - 4180.0) < 1e-3

    # 3. 16 位计数器回绕增量测试
    assert calc_delta_u16(10, 5) == 5
    assert calc_delta_u16(5, 5) == 0
    # 回绕场景: 上一次 0xFFFE, 当前 0x0003 -> delta = 5
    assert calc_delta_u16(0x0003, 0xFFFE) == 5


def test_simulator_maintenance_and_exceptions():
    """测试仿真器维护指令、ADC 校零与异常响应。"""
    sim = DspSimulator(slave=2, protocol_version=0x0103)

    # 1. 清除 CPLD 故障
    sim.set_fault(latch_fault=0x00000010, active_fault=0x00000020)
    req_clear = build_write_single_request(2, ADDR_CLEAR_CPLD_FAULT, KEY_CLEAR_CPLD_FAULT)
    resp_clear = sim.handle_request(req_clear)
    parsed, ok = parse_maintenance_back(resp_clear, ADDR_CLEAR_CPLD_FAULT, KEY_CLEAR_CPLD_FAULT)
    assert ok is True
    assert sim.cpld_fault == 0x00000020

    # 2. ADC 重新校零 (0x0104 = 0xCA1B)
    sim.adc_zero_state = 2  # DONE
    req_adc_cal = build_write_single_request(2, ADDR_ADC_RECALIBRATE, KEY_ADC_RECALIBRATE)
    resp_adc_cal = sim.handle_request(req_adc_cal)
    parsed, ok = parse_maintenance_back(resp_adc_cal, ADDR_ADC_RECALIBRATE, KEY_ADC_RECALIBRATE)
    assert ok is True
    assert sim.adc_zero_state == 1  # 进入 RUNNING

    # 3. START 状态下请求维护命令应被拒绝
    sim.command_holding = CMD_START
    resp_err = sim.handle_request(req_clear)
    parsed_err = parse_response(resp_err)
    assert parsed_err.exception_code == 0x03  # 非法数值/状态

    # 4. 非法地址应返回异常 02
    req_bad_addr = build_write_single_request(2, 0x0108, 0x1234)
    resp_bad_addr = sim.handle_request(req_bad_addr)
    parsed_bad_addr = parse_response(resp_bad_addr)
    assert parsed_bad_addr.exception_code == 0x02


def test_priority_queue_scheduling():
    """测试串口工作线程优先队列机制与防积压、STOP 剔除 START。"""
    worker = SerialWorker()

    # 1. 压入多个请求：FC04 (20)、START (2)、维护 (1)、FC03 (10)
    worker.request_input_block()
    worker.queue_start()
    worker.queue_clear_cpld_fault()
    worker.request_holding_command()

    # 2. 检查 FC04 不会重复堆积
    worker.request_input_block()
    worker.request_input_block()

    # 3. 此时下发 STOP（优先级 0），应自动剔除 START
    worker.queue_stop()

    order = []
    while True:
        req = worker._get_next_request()
        if req is None:
            break
        order.append(req[0])

    expected_order = [
        worker.REQ_STOP,
        worker.REQ_CLEAR_CPLD_FAULT,
        worker.REQ_READ_REQUEST,
        worker.REQ_INPUT_BLOCK,
    ]
    assert order == expected_order, f"实际出队顺序: {order}"


def test_protocol_version_support_helpers():
    """测试协议版本能力判断函数。"""
    assert supports_remote_maintenance(0x0100) is False
    assert supports_remote_maintenance(0x0101) is True
    assert supports_remote_reset(0x0101) is False
    assert supports_remote_reset(0x0102) is True
    assert supports_adc_recalibration(0x0102) is True
    assert supports_v103_extended_registers(0x0102) is False
    assert supports_v103_extended_registers(0x0103) is True


if __name__ == "__main__":
    test_crc16_and_known_frames()
    test_v103_29_registers_and_fallback()
    test_engineering_units_and_calibration()
    test_simulator_maintenance_and_exceptions()
    test_priority_queue_scheduling()
    test_protocol_version_support_helpers()
    print("All protocol V0103 tests passed successfully!")
