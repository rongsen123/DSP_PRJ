"""后台串口通信工作线程与 Modbus RTU 事务处理器。

包含：
- 虚拟 DSP 从站仿真器 DspSimulator（支持 0x0103 协议 29 寄存器、0x0101 清故障、0x0102 复位、0x0104 ADC校零）
- 基于 PriorityQueue 的优先级串口事务调度工作线程 SerialWorker
"""

from __future__ import annotations

import heapq
import itertools
import math
import queue
import random
import time
from typing import Callable

import serial
from PySide6.QtCore import QThread, Signal

from communication.crc16 import verify_crc
from communication.modbus_rtu import (
    ModbusResponse,
    build_read_request,
    build_write_single_request,
    parse_holding_read_response,
    parse_read_response,
    parse_response,
    parse_write_single_response,
    parse_write_single_response_exact,
)
from protocol.register_model import (
    ADDR_ADC_RECALIBRATE,
    ADDR_CLEAR_CPLD_FAULT,
    ADDR_COMMAND,
    ADDR_RESET_CPLD,
    ADDR_RESET_DSP,
    CMD_START,
    CMD_STOP,
    INPUT_QUANTITY_V101,
    INPUT_QUANTITY_V103,
    KEY_ADC_RECALIBRATE,
    KEY_CLEAR_CPLD_FAULT,
    KEY_RESET_CPLD,
    KEY_RESET_DSP,
    PROTOCOL_VERSION_V103,
)

INTER_FRAME_GAP = 0.00175  # 固定 t3.5 = 1.75 ms


class FrameResult:
    """一次收发结果的轻量载体。"""

    def __init__(
        self,
        request: bytes,
        response: bytes | None,
        parsed: ModbusResponse | None,
        extra: object | None,
        elapsed_ms: float,
        error: str | None,
        req_type: str | None = None,
    ):
        self.request = request
        self.response = response
        self.parsed = parsed
        self.extra = extra
        self.elapsed_ms = elapsed_ms
        self.error = error
        self.req_type = req_type


class DspSimulator:
    """虚拟 DSP + CPLD Modbus 从站仿真器。

    支持 0x0103 协议 29 寄存器、STOP/START、CPLD 锁存故障清除、CPLD/DSP 复位和 ADC 重新校零。
    """

    def __init__(self, slave: int = 2, protocol_version: int = PROTOCOL_VERSION_V103):
        self.slave = slave
        self.protocol_version = protocol_version
        self.start_time = time.time()
        self.adc_sample_count = 0
        self.command_holding = 0  # 0=STOP, 1=START
        self.command_echo = 0     # 0=STOP, 1=START
        self.pll_locked = True
        self.pll_frequency = 5000 # 50.00 Hz
        self.pll_signal_valid = True
        self.statcom_state = 1    # ADC_VERIFY
        self.adc_zero_state = 2   # DONE
        self.adc_calibration_samples = 1024
        self.dsp_cpld_errors = 0
        self.pc_dsp_errors = 0
        self.cpld_status = 0x0017 # link_online, adc_valid, temp_valid, pwm_healthy
        self.cpld_fault = 0x00000000
        self.active_fault = 0x00000000  # 硬件持续故障源（不可清除）
        self.cpld_link = 0x0001   # online

        # V0x0103 新增诊断错误计数
        self.cpld_uart_errors = 0
        self.cpld_crc_errors = 0
        self.cpld_incomplete_frame_errors = 0
        self.dsp_scia_format_errors = 0
        self.dsp_scia_overflow_errors = 0
        self.dsp_cpld_timeouts = 0

    def set_fault(self, latch_fault: int = 0, active_fault: int = 0):
        """设置仿真故障源（用于测试清除故障功能）。"""
        self.active_fault = active_fault
        self.cpld_fault = latch_fault | active_fault
        if self.cpld_fault != 0:
            self.cpld_status |= 0x0008  # 置位 fault_any

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
        if self.adc_zero_state == 1:
            self.adc_calibration_samples += 200
            if self.adc_calibration_samples >= 1024:
                self.adc_calibration_samples = 1024
                self.adc_zero_state = 2

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
            if start_addr == 0x0000:
                full_regs = [
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
                    self.cpld_uart_errors,              # 0017
                    self.cpld_crc_errors,               # 0018
                    self.cpld_incomplete_frame_errors,  # 0019
                    self.dsp_scia_format_errors,        # 001A
                    self.dsp_scia_overflow_errors,      # 001B
                    self.dsp_cpld_timeouts,             # 001C
                ]
                if qty <= len(full_regs):
                    regs = full_regs[:qty]
                    byte_count = qty * 2
                    payload = bytearray([slave, func, byte_count])
                    for r in regs:
                        payload.extend([(r >> 8) & 0xFF, r & 0xFF])
                    from communication.crc16 import append_crc
                    return append_crc(bytes(payload))
                else:
                    from communication.crc16 import append_crc
                    return append_crc(bytes([slave, 0x84, 0x02]))

        elif func == 0x03:  # 读保持寄存器
            start_addr = (frame[2] << 8) | frame[3]
            qty = (frame[4] << 8) | frame[5]
            if start_addr == ADDR_COMMAND and qty == 1:
                payload = bytearray([slave, func, 2, (self.command_holding >> 8) & 0xFF, self.command_holding & 0xFF])
                from communication.crc16 import append_crc
                return append_crc(bytes(payload))

        elif func == 0x06:  # 写单个保持寄存器
            addr = (frame[2] << 8) | frame[3]
            val = (frame[4] << 8) | frame[5]
            from communication.crc16 import append_crc

            if addr == ADDR_COMMAND:
                if val in (CMD_STOP, CMD_START):
                    self.command_holding = val
                    self.command_echo = val  # 模拟硬件回显
                    return frame  # FC06 返回原请求帧
                else:
                    return append_crc(bytes([slave, 0x86, 0x03]))

            # 维护寄存器检查版本支持
            if addr in (ADDR_CLEAR_CPLD_FAULT, ADDR_RESET_CPLD,
                        ADDR_RESET_DSP, ADDR_ADC_RECALIBRATE):
                required_version = 0x0101 if addr == ADDR_CLEAR_CPLD_FAULT else 0x0102
                if self.protocol_version < required_version:
                    return append_crc(bytes([slave, 0x86, 0x02]))

                if addr == ADDR_CLEAR_CPLD_FAULT:
                    if val == KEY_CLEAR_CPLD_FAULT and self.command_holding == CMD_STOP:
                        self.cpld_fault = self.active_fault
                        if self.cpld_fault == 0:
                            self.cpld_status &= ~0x0008
                        return frame
                    else:
                        return append_crc(bytes([slave, 0x86, 0x03]))

                elif addr == ADDR_ADC_RECALIBRATE:
                    if (val == KEY_ADC_RECALIBRATE and
                            self.command_holding == CMD_STOP and
                            self.adc_zero_state != 1):
                        self.adc_zero_state = 1
                        self.adc_calibration_samples = 0
                        return frame
                    else:
                        return append_crc(bytes([slave, 0x86, 0x03]))

                elif addr == ADDR_RESET_CPLD:
                    if val == KEY_RESET_CPLD and self.command_holding == CMD_STOP:
                        self.command_echo = CMD_STOP
                        self.cpld_fault = self.active_fault
                        self.cpld_status = 0x0017 | (0x0008 if self.cpld_fault else 0)
                        self.cpld_link = 0x0001
                        return frame
                    return append_crc(bytes([slave, 0x86, 0x03]))

                elif addr == ADDR_RESET_DSP:
                    if val == KEY_RESET_DSP and self.command_holding == CMD_STOP:
                        self.start_time = time.time()
                        self.adc_sample_count = 0
                        self.command_holding = CMD_STOP
                        self.command_echo = CMD_STOP
                        self.statcom_state = 1
                        self.adc_zero_state = 1
                        self.adc_calibration_samples = 0
                        return frame
                    return append_crc(bytes([slave, 0x86, 0x03]))

            # 其他未实现地址返回异常 02
            return append_crc(bytes([slave, 0x86, 0x02]))

        return None


class SerialWorker(QThread):
    """串口工作线程，处理普通读取与命令写入。

    使用线程安全 PriorityQueue 进行调度：
      - 优先级 0: STOP（最高优先级，入队时丢弃待发 START）
      - 优先级 1: 维护命令（清故障 / CPLD复位 / DSP复位 / ADC重新校零）
      - 优先级 2: START
      - 优先级 10: FC03 手动读取
      - 优先级 20: 周期 FC04 读取（单例入队，不重复堆积）
    """

    reply_ready = Signal(object)          # FrameResult（普通读取结果）
    command_reply = Signal(object)        # FrameResult（STOP/START命令结果）
    maintenance_reply = Signal(object)    # FrameResult（维护命令结果）
    port_error = Signal(str)
    connected = Signal()
    disconnected = Signal()

    PRIO_STOP = 0
    PRIO_MAINTENANCE = 1
    PRIO_START = 2
    PRIO_READ_HOLDING = 10
    PRIO_READ_INPUT = 20

    REQ_STOP = "req_stop"
    REQ_START = "req_start"
    REQ_CLEAR_CPLD_FAULT = "req_clear_cpld_fault"
    REQ_RESET_CPLD = "req_reset_cpld"
    REQ_RESET_DSP = "req_reset_dsp"
    REQ_ADC_RECALIBRATE = "req_adc_recalibrate"
    REQ_READ_REQUEST = "req_read_request"
    REQ_INPUT_BLOCK = "req_input_block"

    def __init__(self, parent=None):
        super().__init__(parent)
        self._port: serial.Serial | None = None
        self._simulator: DspSimulator | None = None
        self._is_simulation = False
        self._running = False
        self._queue: queue.PriorityQueue = queue.PriorityQueue()
        self._seq = itertools.count()
        self._port_name = ""
        self._slave = 2
        self._baud = 115200
        self._timeout = 0.1
        self._input_quantity = INPUT_QUANTITY_V103

    @property
    def input_quantity(self) -> int:
        return self._input_quantity

    def set_input_quantity(self, qty: int):
        """设置周期轮询读取的输入寄存器数量（23 或 29）。"""
        self._input_quantity = qty

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

    # ---------------- 队列调度接口 ----------------

    def queue_stop(self):
        """下发 STOP 命令（最高优先级 0，剔除队列中未发送的 START）。"""
        with self._queue.mutex:
            self._queue.queue = [item for item in self._queue.queue if item[2] != self.REQ_START]
            heapq.heapify(self._queue.queue)
        self._queue.put((self.PRIO_STOP, next(self._seq), self.REQ_STOP, CMD_STOP))

    def queue_start(self):
        """下发 START 命令（优先级 2）。"""
        self._queue.put((self.PRIO_START, next(self._seq), self.REQ_START, CMD_START))

    def queue_clear_cpld_fault(self):
        """下发清除 CPLD 锁存故障命令（优先级 1，地址 0x0101，密钥 0xA55A）。"""
        self._queue.put((self.PRIO_MAINTENANCE, next(self._seq), self.REQ_CLEAR_CPLD_FAULT, KEY_CLEAR_CPLD_FAULT))

    def queue_adc_recalibrate(self):
        """请求 DSP 重新执行电流传感器零漂校准（地址 0x0104，密钥 0xCA1B）。"""
        self._queue.put((self.PRIO_MAINTENANCE, next(self._seq), self.REQ_ADC_RECALIBRATE, KEY_ADC_RECALIBRATE))

    def queue_reset_cpld(self):
        """请求 DSP 转发 CPLD 软复位命令（0x0102=0xC33C）。"""
        self._queue.put((self.PRIO_MAINTENANCE, next(self._seq), self.REQ_RESET_CPLD, KEY_RESET_CPLD))

    def queue_reset_dsp(self):
        """请求 DSP 通过看门狗软复位（0x0103=0xD55D）。"""
        self._queue.put((self.PRIO_MAINTENANCE, next(self._seq), self.REQ_RESET_DSP, KEY_RESET_DSP))

    def request_input_block(self, qty: int | None = None):
        """请求输入寄存器块（周期 FC04，优先级 20，防积压）。"""
        target_qty = qty if qty is not None else self._input_quantity
        with self._queue.mutex:
            has_input_block = any(item[2] == self.REQ_INPUT_BLOCK for item in self._queue.queue)
        if not has_input_block:
            self._queue.put((self.PRIO_READ_INPUT, next(self._seq), self.REQ_INPUT_BLOCK, target_qty))

    def request_holding_command(self):
        """请求读单个保持寄存器（FC03，优先级 10）。"""
        self._queue.put((self.PRIO_READ_HOLDING, next(self._seq), self.REQ_READ_REQUEST, None))

    def queue_write_command(self, value: int):
        """向后兼容写命令接口：0 -> STOP, 1 -> START。"""
        if value == 0:
            self.queue_stop()
        else:
            self.queue_start()

    # ---------------- 内部轮询与执行 ----------------

    def _get_next_request(self) -> tuple[str, object | None] | None:
        try:
            prio, seq, req_type, payload = self._queue.get_nowait()
            return req_type, payload
        except queue.Empty:
            return None

    def _poll_pending_sim(self):
        item = self._get_next_request()
        if item is None or not self._simulator:
            return
        req_type, payload = item

        start = time.monotonic()
        if req_type == self.REQ_INPUT_BLOCK:
            qty = int(payload) if isinstance(payload, int) else self._input_quantity
            frame = build_read_request(self._slave, 0x04, 0x0000, qty)
            resp = self._simulator.handle_request(frame)
            elapsed = (time.monotonic() - start) * 1000.0 + random.uniform(2.0, 5.0)
            parsed, extra = parse_input_block(resp)
            self.reply_ready.emit(FrameResult(frame, resp, parsed, extra, elapsed, None, req_type))

        elif req_type == self.REQ_READ_REQUEST:
            frame = build_read_request(self._slave, 0x03, ADDR_COMMAND, 1)
            resp = self._simulator.handle_request(frame)
            elapsed = (time.monotonic() - start) * 1000.0 + random.uniform(2.0, 4.0)
            parsed, extra = parse_read_back(resp)
            self.reply_ready.emit(FrameResult(frame, resp, parsed, extra, elapsed, None, req_type))

        elif req_type in (self.REQ_STOP, self.REQ_START):
            frame = build_write_single_request(self._slave, ADDR_COMMAND, int(payload))
            resp = self._simulator.handle_request(frame)
            elapsed = (time.monotonic() - start) * 1000.0 + random.uniform(2.0, 4.0)
            parsed, extra = parse_write_back(resp)
            self.command_reply.emit(FrameResult(frame, resp, parsed, extra, elapsed, None, req_type))

        elif req_type == self.REQ_CLEAR_CPLD_FAULT:
            frame = build_write_single_request(self._slave, ADDR_CLEAR_CPLD_FAULT, KEY_CLEAR_CPLD_FAULT)
            resp = self._simulator.handle_request(frame)
            elapsed = (time.monotonic() - start) * 1000.0 + random.uniform(2.0, 4.0)
            parsed, ok = parse_maintenance_back(resp, ADDR_CLEAR_CPLD_FAULT, KEY_CLEAR_CPLD_FAULT)
            self.maintenance_reply.emit(FrameResult(frame, resp, parsed, ok, elapsed, None, req_type))

        elif req_type == self.REQ_ADC_RECALIBRATE:
            frame = build_write_single_request(self._slave, ADDR_ADC_RECALIBRATE, KEY_ADC_RECALIBRATE)
            resp = self._simulator.handle_request(frame)
            elapsed = (time.monotonic() - start) * 1000.0 + random.uniform(2.0, 4.0)
            parsed, ok = parse_maintenance_back(resp, ADDR_ADC_RECALIBRATE, KEY_ADC_RECALIBRATE)
            self.maintenance_reply.emit(FrameResult(frame, resp, parsed, ok, elapsed, None, req_type))

        elif req_type == self.REQ_RESET_CPLD:
            frame = build_write_single_request(self._slave, ADDR_RESET_CPLD, KEY_RESET_CPLD)
            resp = self._simulator.handle_request(frame)
            elapsed = (time.monotonic() - start) * 1000.0 + random.uniform(2.0, 4.0)
            parsed, ok = parse_maintenance_back(resp, ADDR_RESET_CPLD, KEY_RESET_CPLD)
            self.maintenance_reply.emit(FrameResult(frame, resp, parsed, ok, elapsed, None, req_type))

        elif req_type == self.REQ_RESET_DSP:
            frame = build_write_single_request(self._slave, ADDR_RESET_DSP, KEY_RESET_DSP)
            resp = self._simulator.handle_request(frame)
            elapsed = (time.monotonic() - start) * 1000.0 + random.uniform(2.0, 4.0)
            parsed, ok = parse_maintenance_back(resp, ADDR_RESET_DSP, KEY_RESET_DSP)
            self.maintenance_reply.emit(FrameResult(frame, resp, parsed, ok, elapsed, None, req_type))

    def _poll_pending(self):
        item = self._get_next_request()
        if item is None:
            return
        req_type, payload = item

        try:
            if req_type == self.REQ_INPUT_BLOCK:
                qty = int(payload) if isinstance(payload, int) else self._input_quantity
                frame = build_read_request(self._slave, 0x04, 0x0000, qty)
                result = self._transact(frame, parse_input_block, req_type)
                self.reply_ready.emit(result)

            elif req_type == self.REQ_READ_REQUEST:
                frame = build_read_request(self._slave, 0x03, ADDR_COMMAND, 1)
                result = self._transact(frame, parse_read_back, req_type)
                self.reply_ready.emit(result)

            elif req_type in (self.REQ_STOP, self.REQ_START):
                frame = build_write_single_request(self._slave, ADDR_COMMAND, int(payload))
                result = self._transact(frame, parse_write_back, req_type)
                self.command_reply.emit(result)

            elif req_type == self.REQ_CLEAR_CPLD_FAULT:
                frame = build_write_single_request(self._slave, ADDR_CLEAR_CPLD_FAULT, KEY_CLEAR_CPLD_FAULT)
                result = self._transact(
                    frame,
                    lambda r: parse_maintenance_back(r, ADDR_CLEAR_CPLD_FAULT, KEY_CLEAR_CPLD_FAULT),
                    req_type,
                )
                self.maintenance_reply.emit(result)

            elif req_type == self.REQ_ADC_RECALIBRATE:
                frame = build_write_single_request(self._slave, ADDR_ADC_RECALIBRATE, KEY_ADC_RECALIBRATE)
                result = self._transact(
                    frame,
                    lambda r: parse_maintenance_back(r, ADDR_ADC_RECALIBRATE, KEY_ADC_RECALIBRATE),
                    req_type,
                )
                self.maintenance_reply.emit(result)

            elif req_type == self.REQ_RESET_CPLD:
                frame = build_write_single_request(self._slave, ADDR_RESET_CPLD, KEY_RESET_CPLD)
                result = self._transact(
                    frame,
                    lambda r: parse_maintenance_back(r, ADDR_RESET_CPLD, KEY_RESET_CPLD),
                    req_type,
                )
                self.maintenance_reply.emit(result)

            elif req_type == self.REQ_RESET_DSP:
                frame = build_write_single_request(self._slave, ADDR_RESET_DSP, KEY_RESET_DSP)
                result = self._transact(
                    frame,
                    lambda r: parse_maintenance_back(r, ADDR_RESET_DSP, KEY_RESET_DSP),
                    req_type,
                )
                self.maintenance_reply.emit(result)

        except Exception as exc:
            err_result = FrameResult(b"", None, None, None, 0.0, str(exc), req_type)
            if req_type in (self.REQ_STOP, self.REQ_START):
                self.command_reply.emit(err_result)
            elif req_type in (self.REQ_CLEAR_CPLD_FAULT, self.REQ_RESET_CPLD,
                               self.REQ_RESET_DSP, self.REQ_ADC_RECALIBRATE):
                self.maintenance_reply.emit(err_result)
            else:
                self.reply_ready.emit(err_result)

    def _transact(self, frame: bytes, parser: Callable, req_type: str | None = None) -> FrameResult:
        if not self._port or not self._port.is_open:
            return FrameResult(frame, None, None, None, 0.0, "串口未打开", req_type)
        start = time.monotonic()
        self._port.reset_input_buffer()
        self._port.write(frame)
        response = self._read_frame()
        elapsed = (time.monotonic() - start) * 1000.0
        if response is None or not response:
            return FrameResult(frame, None, None, None, elapsed, "超时未收到响应", req_type)
        parsed, extra = parser(response)
        return FrameResult(frame, response, parsed, extra, elapsed, None, req_type)

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


def parse_input_block(response: bytes | None):
    """解析 FC04 输入寄存器响应（自动识别 23 或 29 寄存器）。"""
    if response is None:
        return None, None
    parsed = parse_response(response)
    values = parse_read_response(parsed, None)
    return parsed, values


def parse_read_back(response: bytes | None):
    if response is None:
        return None, None
    parsed = parse_response(response)
    value = parse_holding_read_response(parsed)
    return parsed, value


def parse_write_back(response: bytes | None):
    if response is None:
        return None, None
    parsed = parse_response(response)
    value = parse_write_single_response(parsed)
    return parsed, value


def parse_maintenance_back(response: bytes | None, expected_address: int, expected_key: int):
    """解析维护命令响应帧，返回 (parsed, is_success)。"""
    if response is None:
        return None, False
    parsed = parse_response(response)
    if parsed.error or parsed.exception_code is not None:
        return parsed, False
    is_match = parse_write_single_response_exact(parsed, expected_address, expected_key)
    return parsed, is_match
