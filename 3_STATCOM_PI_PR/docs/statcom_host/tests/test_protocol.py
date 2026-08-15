import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from communication.crc16 import crc16_modbus, append_crc, verify_crc
from communication.modbus_rtu import (
    build_read_request,
    build_write_single_request,
    parse_response,
    parse_read_response,
)
from communication.serial_worker import DspSimulator, parse_input_block
from protocol.register_model import parse_input_registers, to_signed, uint32


def test_crc16():
    # 规范第 5.3 节已知帧
    # 读 23 个寄存器：02 04 00 00 00 17 -> CRC = B0 37 (低位在前: 37 B0)
    data = bytes([0x02, 0x04, 0x00, 0x00, 0x00, 0x17])
    crc = crc16_modbus(data)
    assert crc == 0x37B0
    appended = append_crc(data)
    assert appended == bytes([0x02, 0x04, 0x00, 0x00, 0x00, 0x17, 0xB0, 0x37])
    assert verify_crc(appended) is True


def test_modbus_build_and_sim():
    sim = DspSimulator(slave=2)
    # 构造 FC04 请求
    req = build_read_request(2, 0x04, 0x0000, 23)
    resp = sim.handle_request(req)
    assert resp is not None
    assert verify_crc(resp) is True

    parsed, values = parse_input_block(resp)
    assert parsed.error is None
    assert values is not None
    assert len(values) == 23

    # 解析寄存器模型
    data = parse_input_registers(list(values))
    assert data["protocol_version"] == 0x0100
    assert data["pll_frequency"] == 50.0 or abs(data["pll_frequency"] - 50.0) < 1.0


def test_signed_and_uint32():
    assert to_signed(0x0000) == 0
    assert to_signed(0x0064) == 100
    assert to_signed(0xFF9C) == -100
    assert uint32(0x1234, 0x5678) == 0x12345678


if __name__ == "__main__":
    test_crc16()
    test_modbus_build_and_sim()
    test_signed_and_uint32()
    print("All tests passed successfully!")
