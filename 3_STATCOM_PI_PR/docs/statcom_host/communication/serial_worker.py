"""串口通信工作线程及 DSP 硬件仿真器。

串口收发全部在此线程完成，禁止在 UI 线程阻塞读串口。
内置仿真模式（SIMULATOR），无硬件时可完整仿真单相 STATCOM 运行数据。
"""

from __future__ import annotations

import math
import random
import time

import serial
from PySide6.QtCore import QThread, Signal

from communication.modbus_rtu import (
    build_read_request,
    build_write_single_request,
    parse_response,
    parse_read_response,
    parse_holding_read_response,
    parse_write_single_response,
    ModbusResponse,
)

INTER_FRAME_GAP = 0.00175  # 固定 t3.5 = 1.75 ms


class FrameResult:
    """一次收发结果的轻量载体。"""

    def __init__(self, request: bytes, response: bytes | None, parsed: ModbusResponse | None,
                 extra: object | None, elapsed_ms: float, error: str | None):
        self.request = request
        self.response = response
        self.parsed = parsed
        self.extra = extra
        self.elapsed_ms = elapsed_ms
        self.error = error


class DspSimulator:
    """虚拟 DSP + CPLD Modbus 从站仿真器。"""

    def __init__(self, slave: int = 2):
        self.slave = slave
        self.protocol_version = 0x0100
        self.start_time = time.time()
        self.adc_sample_count = 0
        self.command_holding = 0  # 0=STOP, 1=START
        self.command_echo = 0     # 0=STOP, 1=START
        self.pll_locked = True
        self.pll_frequency = 5000 # 50.00 Hz
        self.pll_signal_valid = True
        self.statcom_state = 1    # ADC_VERIFY
        self.adc_zero_state = 2   # DONE
        self.dsp_cpld_errors = 0
        self.pc_dsp_errors = 0
        self.cpld_status = 0x0017 # link_online, adc_valid, temp_valid, pwm_healthy
        self.cpld_fault = 0x00000000
        self.cpld_link = 0x0001   # online

    def handle_request(self, frame: bytes) -> bytes | None:
        """处理 Modbus RTU 请求帧并生成模拟响应。"""
        if len(frame) < 4:
            return None
        slave = frame[0]
        if slave != self.slave:
            return None
        func = frame[1]

        # 模拟运行时间与波形演进
        now = time.time()
        uptime_ms = int((now - self.start_time) * 1000) & 0xFFFFFFFF
        self.adc_sample_count = (self.adc_sample_count + 200) & 0xFFFFFFFF

        t = now - self.start_time
        # 电网正半波估算采样 (约 220V * sqrt(2) 正弦半波，映射到 0~4095 码)
        sin_val = max(0.0, math.sin(2 * math.pi * 50.0 * t))
        grid_raw = int(sin_val * 1130 + random.uniform(-5, 5))
        grid_raw = max(0, min(4095, grid_raw))

        # 交流电流采样 (A)
        if self.command_echo == 1:
            iac_val = 5.0 * math.sin(2 * math.pi * 50.0 * t - 0.2) + random.uniform(-0.1, 0.1)
            self.statcom_state = 7  # RUN
        else:
            iac_val = random.uniform(-0.05, 0.05)
            self.statcom_state = 3  # PLL_MONITOR

        iac_scaled = int(iac_val * 100) & 0xFFFF
        iac_raw = int(2048 + iac_val * 10.11) & 0x0FFF

        # 直流电压 Vdc (约 350V 对应 count)
        vdc_avg = int(1276 + (15 if self.command_echo == 1 else 0) + random.uniform(-2, 2))
        vdc_raw = int(vdc_avg + random.uniform(-4, 4))

        # 温度计数
        temp_count = int(1050 + random.uniform(-5, 5))

        if func == 0x04:  # 读输入寄存器
            start_addr = (frame[2] << 8) | frame[3]
            qty = (frame[4] << 8) | frame[5]
            if start_addr == 0x0000 and qty == 23:
                regs = [
                    self.protocol_version,              # 0000
                    (uptime_ms >> 16) & 0xFFFF,         # 0001
                    uptime_ms & 0xFFFF,                 # 0002
                    (self.adc_sample_count >> 16) & 0xFFFF, # 0003
                    self.adc_sample_count & 0xFFFF,     # 0004
                    grid_raw,                           # 0005
                    iac_raw,                            # 0006
                    iac_scaled,                         # 0007
                    vdc_raw,                            # 0008
                    vdc_avg,                            # 0009
                    temp_count,                         # 000A
                    self.cpld_status,                   # 000B
                    (self.cpld_fault >> 16) & 0xFFFF,   # 000C
                    self.cpld_fault & 0xFFFF,           # 000D
                    self.cpld_link,                     # 000E
                    1 if self.pll_locked else 0,        # 000F
                    self.pll_frequency + int(random.uniform(-5, 5)), # 0010 (50.00 Hz)
                    1 if self.pll_signal_valid else 0,  # 0011
                    self.statcom_state,                 # 0012
                    self.adc_zero_state,                # 0013
                    self.dsp_cpld_errors,               # 0014
                    self.pc_dsp_errors,                 # 0015
                    self.command_echo,                  # 0016
                ]
                byte_count = qty * 2
                payload = bytearray([slave, func, byte_count])
                for r in regs:
                    payload.extend([(r >> 8) & 0xFF, r & 0xFF])
                from communication.crc16 import append_crc
                return append_crc(bytes(payload))

        elif func == 0x03:  # 读保持寄存器
            start_addr = (frame[2] << 8) | frame[3]
            qty = (frame[4] << 8) | frame[5]
            if start_addr == 0x0100 and qty == 1:
                payload = bytearray([slave, func, 2, (self.command_holding >> 8) & 0xFF, self.command_holding & 0xFF])
                from communication.crc16 import append_crc
                return append_crc(bytes(payload))

        elif func == 0x06:  # 写单个保持寄存器
            addr = (frame[2] << 8) | frame[3]
            val = (frame[4] << 8) | frame[5]
            if addr == 0x0100:
                if val in (0, 1):
                    self.command_holding = val
                    self.command_echo = val  # 模拟硬件回显
                    return frame  # FC06 返回原请求帧
                else:
                    # 异常 03 (非法值)
                    from communication.crc16 import append_crc
                    return append_crc(bytes([slave, 0x86, 0x03]))
            else:
                # 异常 02 (非法地址)
                from communication.crc16 import append_crc
                return append_crc(bytes([slave, 0x86, 0x02]))

        return None


class SerialWorker(QThread):
    """串口工作线程，处理普通读取与命令写入。
    普通请求以队列入栈，命令写入具有优先级（STOP 需中断排队普通读取）。
    """

    reply_ready = Signal(object)          # FrameResult（普通读取结果）
    command_reply = Signal(object)        # FrameResult（命令写入结果）
    port_error = Signal(str)
    connected = Signal()
    disconnected = Signal()

    REQ_INPUT_BLOCK = "req_input_block"
    REQ_READ_REQUEST = "req_read_request"
    REQ_WRITE_CMD = "req_write_cmd"

    def __init__(self, parent=None):
        super().__init__(parent)
        self._port: serial.Serial | None = None
        self._simulator: DspSimulator | None = None
        self._is_simulation = False
        self._running = False
        self._pending_request: tuple[str, object | None] | None = None
        self._port_name = ""
        self._slave = 2
        self._baud = 115200
        self._timeout = 0.1

    def configure(self, port_name: str, slave: int, baud=115200, timeout=0.1, simulation=False):
        self._port_name = port_name
        self._slave = slave
        self._baud = baud
        self._timeout = timeout
        self._is_simulation = simulation or ("仿真" in port_name or port_name == "SIMULATOR")

    def run(self):
        self._running = True
        if self._is_simulation:
            self._simulator = DspSimulator(slave=self._slave)
            self.connected.emit()
            while self._running:
                self._poll_pending_sim()
                self.msleep(10)
            self._simulator = None
            self.disconnected.emit()
            return

        try:
            self._port = serial.Serial(
                port=self._port_name,
                baudrate=self._baud,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=self._timeout,
            )
        except Exception as exc:
            self.port_error.emit(str(exc))
            self._running = False
            self._port = None
            return

        self.connected.emit()
        while self._running:
            self._poll_pending()
            self.msleep(2)

        if self._port and self._port.is_open:
            self._port.close()
        self._port = None
        self.disconnected.emit()

    def stop(self):
        self._running = False
        self.wait(500)

    def request_input_block(self):
        self._pending_request = (self.REQ_INPUT_BLOCK, None)

    def request_holding_command(self):
        self._pending_request = (self.REQ_READ_REQUEST, None)

    def queue_write_command(self, value: int):
        """优先级写命令：覆盖普通读取。"""
        self._pending_request = (self.REQ_WRITE_CMD, value)

    def _poll_pending_sim(self):
        if self._pending_request is None or not self._simulator:
            return
        req_type, payload = self._pending_request
        self._pending_request = None

        start = time.monotonic()
        if req_type == self.REQ_INPUT_BLOCK:
            frame = build_read_request(self._slave, 0x04, 0x0000, 23)
            resp = self._simulator.handle_request(frame)
            elapsed = (time.monotonic() - start) * 1000.0 + random.uniform(2.0, 5.0)
            parsed, extra = parse_input_block(resp)
            self.reply_ready.emit(FrameResult(frame, resp, parsed, extra, elapsed, None))
        elif req_type == self.REQ_READ_REQUEST:
            frame = build_read_request(self._slave, 0x03, 0x0100, 1)
            resp = self._simulator.handle_request(frame)
            elapsed = (time.monotonic() - start) * 1000.0 + random.uniform(2.0, 4.0)
            parsed, extra = parse_read_back(resp)
            self.reply_ready.emit(FrameResult(frame, resp, parsed, extra, elapsed, None))
        elif req_type == self.REQ_WRITE_CMD:
            frame = build_write_single_request(self._slave, 0x0100, int(payload))
            resp = self._simulator.handle_request(frame)
            elapsed = (time.monotonic() - start) * 1000.0 + random.uniform(2.0, 4.0)
            parsed, extra = parse_write_back(resp)
            self.command_reply.emit(FrameResult(frame, resp, parsed, extra, elapsed, None))

    def _poll_pending(self):
        if self._pending_request is None:
            return
        req_type, payload = self._pending_request
        self._pending_request = None

        try:
            if req_type == self.REQ_INPUT_BLOCK:
                frame = build_read_request(self._slave, 0x04, 0x0000, 23)
                result = self._transact(frame, parse_input_block)
                self.reply_ready.emit(result)
            elif req_type == self.REQ_READ_REQUEST:
                frame = build_read_request(self._slave, 0x03, 0x0100, 1)
                result = self._transact(frame, parse_read_back)
                self.reply_ready.emit(result)
            elif req_type == self.REQ_WRITE_CMD:
                frame = build_write_single_request(self._slave, 0x0100, int(payload))
                result = self._transact(frame, parse_write_back)
                self.command_reply.emit(result)
        except Exception as exc:
            self.command_reply.emit(FrameResult(b"", None, None, None, 0.0, str(exc)))

    def _transact(self, frame: bytes, parser) -> FrameResult:
        if not self._port or not self._port.is_open:
            return FrameResult(frame, None, None, None, 0.0, "串口未打开")
        start = time.monotonic()
        self._port.reset_input_buffer()
        self._port.write(frame)
        response = self._read_frame()
        elapsed = (time.monotonic() - start) * 1000.0
        if response is None or not response:
            return FrameResult(frame, None, None, None, elapsed, "超时未收到响应")
        parsed, extra = parser(response)
        return FrameResult(frame, response, parsed, extra, elapsed, None)

    def _read_frame(self) -> bytes | None:
        if not self._port:
            return None
        head = self._port.read(1)
        if not head:
            return None
        buf = bytearray(head)
        func = self._port.read(1)
        if not func:
            return None
        buf += func
        if func[0] & 0x80:
            rest = self._port.read(3)
            buf += rest
            return bytes(buf)
        if func[0] in (0x03, 0x04):
            bc = self._port.read(1)
            if not bc:
                return None
            buf += bc
            data_len = bc[0]
            data = self._read_exact(data_len + 2)
            if data is None:
                return None
            buf += data
            return bytes(buf)
        if func[0] in (0x06, 0x10):
            data = self._read_exact(6)
            if data is None:
                return None
            buf += data
            return bytes(buf)
        return None

    def _read_exact(self, n: int) -> bytes | None:
        if not self._port:
            return None
        data = self._port.read(n)
        return data if len(data) == n else None


def parse_input_block(response: bytes):
    parsed = parse_response(response)
    values = parse_read_response(parsed, 23)
    return parsed, values


def parse_read_back(response: bytes):
    parsed = parse_response(response)
    value = parse_holding_read_response(parsed)
    return parsed, value


def parse_write_back(response: bytes):
    parsed = parse_response(response)
    value = parse_write_single_response(parsed)
    return parsed, value