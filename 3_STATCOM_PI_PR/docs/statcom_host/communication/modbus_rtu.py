"""Modbus RTU 协议帧构建与解析（纯函数，便于单元测试）。"""

from __future__ import annotations

from dataclasses import dataclass

from communication.crc16 import append_crc, verify_crc

# 功能码
FC_READ_HOLDING = 0x03
FC_READ_INPUT = 0x04
FC_WRITE_SINGLE = 0x06
FC_WRITE_MULTIPLE = 0x10

# 异常码
EXC_ILLEGAL_FUNCTION = 0x01
EXC_ILLEGAL_ADDRESS = 0x02
EXC_ILLEGAL_VALUE = 0x03

EXCEPTION_OFFSET = 0x80

EXCEPTION_TEXT = {
    EXC_ILLEGAL_FUNCTION: "非法功能码（固件不支持该功能码）",
    EXC_ILLEGAL_ADDRESS: "非法地址（寄存器未实现/地址错误）",
    EXC_ILLEGAL_VALUE: "非法数值或数量",
}


@dataclass
class ModbusResponse:
    """解析后的 Modbus 响应。"""

    raw: bytes
    slave: int = 0
    function: int = 0
    data: bytes = b""
    exception_code: int | None = None
    valid_crc: bool = True
    error: str | None = None


def build_read_request(slave: int, function: int, start_addr: int, quantity: int) -> bytes:
    """构建 FC03/FC04 读请求帧。

    :param slave: 从站地址。
    :param function: 功能码，0x03 或 0x04。
    :param start_addr: PDU 零基起始地址。
    :param quantity: 寄存器数量。
    """
    pdu = bytes([function, (start_addr >> 8) & 0xFF, start_addr & 0xFF,
                 (quantity >> 8) & 0xFF, quantity & 0xFF])
    return append_crc(bytes([slave]) + pdu)


def build_write_single_request(slave: int, address: int, value: int) -> bytes:
    """构建 FC06 写单个保持寄存器请求。"""
    pdu = bytes([FC_WRITE_SINGLE, (address >> 8) & 0xFF, address & 0xFF,
                 (value >> 8) & 0xFF, value & 0xFF])
    return append_crc(bytes([slave]) + pdu)


def build_write_multiple_request(slave: int, start_addr: int, values: list[int]) -> bytes:
    """构建 FC10 写多个保持寄存器请求。"""
    byte_count = len(values) * 2
    pdu = bytearray([
        FC_WRITE_MULTIPLE,
        (start_addr >> 8) & 0xFF,
        start_addr & 0xFF,
        (len(values) >> 8) & 0xFF,
        len(values) & 0xFF,
        byte_count,
    ])
    for v in values:
        pdu.append((v >> 8) & 0xFF)
        pdu.append(v & 0xFF)
    return append_crc(bytes([slave]) + bytes(pdu))


def parse_response(frame: bytes) -> ModbusResponse:
    """解析从站响应帧。

    返回 :class:`ModbusResponse`，包含校验结果与异常信息。
    """
    resp = ModbusResponse(raw=frame)
    if len(frame) < 4:
        resp.valid_crc = False
        resp.error = "帧过短，无法解析"
        return resp

    if not verify_crc(frame):
        resp.valid_crc = False
        resp.error = "CRC 校验错误"
        return resp

    resp.slave = frame[0]
    resp.function = frame[1]

    if resp.function & EXCEPTION_OFFSET:
        exc = frame[2] if len(frame) >= 3 else 0
        resp.exception_code = exc
        resp.error = EXCEPTION_TEXT.get(exc, f"未知异常码 0x{exc:02X}")
        return resp

    resp.data = frame[2:-2]
    return resp


def parse_read_response(response: ModbusResponse, expected_quantity: int) -> tuple[int, ...] | None:
    """从 FC03/FC04 响应中解析寄存器值列表。

    :return: 寄存器值元组，无法解析时返回 ``None``。
    """
    if response.valid_crc is False or response.error or response.exception_code is not None:
        return None
    if response.function not in (FC_READ_HOLDING, FC_READ_INPUT):
        return None
    if len(response.data) < 1:
        return None
    byte_count = response.data[0]
    payload = response.data[1:]
    if len(payload) != byte_count or byte_count != expected_quantity * 2:
        response.error = "响应字节数与请求数量不匹配"
        return None
    values = []
    for i in range(0, byte_count, 2):
        values.append((payload[i] << 8) | payload[i + 1])
    return tuple(values)


def parse_holding_read_response(response: ModbusResponse) -> int | None:
    """从 FC03 响应中解析单个保持寄存器值。"""
    values = parse_read_response(response, 1)
    return values[0] if values else None


def parse_write_single_response(response: ModbusResponse) -> int | None:
    """从 FC06 响应中解析写回地址与值，返回写入值。"""
    if response.valid_crc is False or response.error or response.exception_code is not None:
        return None
    if response.function != FC_WRITE_SINGLE or len(response.data) != 4:
        return None
    value = (response.data[2] << 8) | response.data[3]
    return value
