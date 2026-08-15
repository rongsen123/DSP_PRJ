"""STATCOM 上位机主窗口（完整版）。

遵循《单相STATCOM调试上位机设计与通信协议规范 V1.0》。
"""

from __future__ import annotations

import os
import time
from collections import deque

os.environ.setdefault("PYQTGRAPH_QT_LIB", "PySide6")

import pyqtgraph as pg
from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QCloseEvent, QColor, QFont
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFileDialog,
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QTabWidget,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

try:
    from serial.tools.list_ports import comports
except Exception:  # noqa: BLE001
    comports = None

from communication.serial_worker import FrameResult, SerialWorker
from protocol import register_model as rm
from storage.logger import DataLogger
from ui.theme import build_stylesheet
from ui.widgets import LedIndicator, StatCard, format_uptime, state_color

HISTORY_LEN = 300
POLL_DEFAULT_MS = 100


class MainWindow(QMainWindow):
    """STATCOM 上位机监控系统主窗口。"""

    def __init__(self):
        super().__init__()
        self.setWindowTitle("单相 STATCOM 调试监控系统 V1.0")
        self.resize(1400, 880)
        self.setMinimumSize(1150, 720)
        self.setStyleSheet(build_stylesheet())

        self._worker: SerialWorker | None = None
        self._connected = False
        self._last_poll_ms = 0.0
        self._dsp_request_cmd = 0
        self._latest_data: dict | None = None
        self._latest_raw: list[int] | None = None

        # 数据记录器
        self._logger = DataLogger(output_dir="data_logs")

        # 统计计数
        self._tx_count = 0
        self._rx_count = 0
        self._crc_err_count = 0
        self._timeout_count = 0

        # 波形缓冲
        self._t_buffer: deque[float] = deque(maxlen=HISTORY_LEN)
        self._iac_buffer: deque[float] = deque(maxlen=HISTORY_LEN)
        self._vdc_buffer: deque[float] = deque(maxlen=HISTORY_LEN)
        self._vgrid_buffer: deque[float] = deque(maxlen=HISTORY_LEN)
        self._pll_buffer: deque[float] = deque(maxlen=HISTORY_LEN)
        self._plot_paused = False

        self._build_ui()
        self._refresh_ports()

        # 轮询定时器
        self._poll_timer = QTimer(self)
        self._poll_timer.setInterval(POLL_DEFAULT_MS)
        self._poll_timer.timeout.connect(self._on_poll_tick)
        self._poll_timer.start()

        # 界面时钟
        self._clock_timer = QTimer(self)
        self._clock_timer.setInterval(1000)
        self._clock_timer.timeout.connect(self._update_clock)
        self._clock_timer.start()
        self._update_clock()

        self._update_connection_ui()

    def _build_ui(self):
        root = QWidget()
        root.setObjectName("Root")
        self.setCentralWidget(root)

        outer = QVBoxLayout(root)
        outer.setContentsMargins(12, 12, 12, 12)
        outer.setSpacing(10)

        outer.addLayout(self._build_top_bar())
        outer.addWidget(self._build_tabs(), 1)
        outer.addWidget(self._build_bottom_bar())

    def _panel(self, title: str) -> tuple[QFrame, QVBoxLayout]:
        frame = QFrame()
        frame.setObjectName("Panel")
        layout = QVBoxLayout(frame)
        layout.setContentsMargins(12, 10, 12, 10)
        layout.setSpacing(8)
        header = QLabel(title)
        header.setObjectName("PanelTitle")
        layout.addWidget(header)
        return frame, layout

    # ------------------------------------------------------------------
    # 顶部工具栏
    # ------------------------------------------------------------------
    def _build_top_bar(self) -> QHBoxLayout:
        bar = QHBoxLayout()
        bar.setSpacing(10)

        title = QLabel("STATCOM 调试监控系统")
        title.setStyleSheet("font-size:18px;font-weight:700;color:#e8edf2;")
        bar.addWidget(title)

        ver_badge = QLabel("V1.0 (DSP 28062 + CPLD)")
        ver_badge.setStyleSheet("color:#6c8299;font-size:12px;margin-right:12px;")
        bar.addWidget(ver_badge)

        bar.addStretch(1)

        # 串口选择
        port_label = QLabel("端口:")
        port_label.setStyleSheet("color:#8fa1b3;font-size:13px;")
        bar.addWidget(port_label)

        self._port_combo = QComboBox()
        self._port_combo.setMinimumWidth(150)
        bar.addWidget(self._port_combo)

        self._refresh_btn = QPushButton("扫描")
        self._refresh_btn.setFixedWidth(55)
        self._refresh_btn.clicked.connect(self._refresh_ports)
        bar.addWidget(self._refresh_btn)

        baud_label = QLabel("波特率:")
        baud_label.setStyleSheet("color:#8fa1b3;font-size:13px;")
        bar.addWidget(baud_label)

        self._baud_combo = QComboBox()
        self._baud_combo.addItems(["115200", "57600", "38400", "19200", "9600"])
        self._baud_combo.setCurrentText("115200")
        self._baud_combo.setFixedWidth(85)
        bar.addWidget(self._baud_combo)

        slave_label = QLabel("从站:")
        slave_label.setStyleSheet("color:#8fa1b3;font-size:13px;")
        bar.addWidget(slave_label)

        self._slave_spin = QComboBox()
        for i in range(1, 16):
            self._slave_spin.addItem(str(i))
        self._slave_spin.setCurrentText("2")
        self._slave_spin.setFixedWidth(50)
        bar.addWidget(self._slave_spin)

        self._connect_btn = QPushButton("连接")
        self._connect_btn.setObjectName("PrimaryBtn")
        self._connect_btn.setMinimumWidth(80)
        self._connect_btn.clicked.connect(self._toggle_connect)
        bar.addWidget(self._connect_btn)

        self._conn_led = LedIndicator("未连接")
        self._conn_led.setFixedWidth(100)
        bar.addWidget(self._conn_led)

        return bar

    # ------------------------------------------------------------------
    # 中间标签页
    # ------------------------------------------------------------------
    def _build_tabs(self) -> QTabWidget:
        tabs = QTabWidget()
        tabs.setObjectName("MainTabs")

        tabs.addTab(self._build_dashboard_tab(), "📊 实时总览")
        tabs.addTab(self._build_trends_tab(), "📈 趋势波形")
        tabs.addTab(self._build_faults_tab(), "⚠️ 故障与事件")
        tabs.addTab(self._build_diag_tab(), "🔍 通信诊断")
        return tabs

    # --- 标签 1: 实时总览 ---
    def _build_dashboard_tab(self) -> QWidget:
        container = QWidget()
        layout = QHBoxLayout(container)
        layout.setContentsMargins(4, 8, 4, 4)
        layout.setSpacing(12)

        # 左侧：状态指示
        left_box = QWidget()
        left_box.setFixedWidth(300)
        left_layout = QVBoxLayout(left_box)
        left_layout.setContentsMargins(0, 0, 0, 0)
        left_layout.setSpacing(10)

        # 调试状态卡片
        state_frame, state_layout = self._panel("DSP 系统状态")
        self._state_value = QLabel("未连接")
        self._state_value.setObjectName("StateValue")
        self._state_value.setAlignment(Qt.AlignmentFlag.AlignCenter)
        state_layout.addWidget(self._state_value)

        self._adc_zero_label = QLabel("ADC 校零：--")
        self._adc_zero_label.setObjectName("Caption")
        self._adc_zero_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        state_layout.addWidget(self._adc_zero_label)

        self._cmd_status_box = QLabel("命令状态：未发送")
        self._cmd_status_box.setStyleSheet("color:#6c8299;font-size:12px;")
        self._cmd_status_box.setAlignment(Qt.AlignmentFlag.AlignCenter)
        state_layout.addWidget(self._cmd_status_box)

        left_layout.addWidget(state_frame)

        # CPLD 状态
        cpld_frame, cpld_layout = self._panel("CPLD 硬件状态")
        self._status_leds: dict[str, LedIndicator] = {}
        for key, label in (
            ("link_online", "DSP-CPLD 在线"),
            ("adc_valid", "CPLD ADC 有效"),
            ("temperature_valid", "温度计数有效"),
            ("fault_any", "存在锁存故障"),
            ("pwm_healthy", "PWM 监测健康"),
        ):
            led = LedIndicator(label)
            self._status_leds[key] = led
            cpld_layout.addWidget(led)
        left_layout.addWidget(cpld_frame)

        # 锁相环 (PLL)
        pll_frame, pll_layout = self._panel("SOGI-PLL 锁相状态")
        self._pll_locked_led = LedIndicator("已锁定网侧相位")
        self._pll_valid_led = LedIndicator("输入信号幅值有效")
        pll_layout.addWidget(self._pll_locked_led)
        pll_layout.addWidget(self._pll_valid_led)
        left_layout.addWidget(pll_frame)
        left_layout.addStretch(1)

        layout.addWidget(left_box)

        # 右侧：核心物理量卡片网格
        right_box = QWidget()
        right_layout = QVBoxLayout(right_box)
        right_layout.setContentsMargins(0, 0, 0, 0)
        right_layout.setSpacing(10)

        grid = QGridLayout()
        grid.setSpacing(12)

        self._card_grid_v = StatCard("电网估算电压", "V (未标定)")
        self._card_iac = StatCard("并网交流电流 Iac", "A")
        self._card_vdc = StatCard("CPLD 直流电压 Vdc", "count (未标定)")
        self._card_pll_f = StatCard("网侧频率 Freq", "Hz")
        self._card_temp = StatCard("散热器温度", "pulse/100ms")
        self._card_adc_count = StatCard("ADC 采样累计计数", "点")

        grid.addWidget(self._card_grid_v, 0, 0)
        grid.addWidget(self._card_iac, 0, 1)
        grid.addWidget(self._card_vdc, 0, 2)
        grid.addWidget(self._card_pll_f, 1, 0)
        grid.addWidget(self._card_temp, 1, 1)
        grid.addWidget(self._card_adc_count, 1, 2)

        right_layout.addLayout(grid)

        # 安全警示横幅
        banner = QFrame()
        banner.setStyleSheet(
            "background-color:#161d26;border:1px solid #2d3e54;border-radius:6px;padding:6px 12px;"
        )
        banner_layout = QHBoxLayout(banner)
        shield_icon = QLabel("🛡️")
        shield_icon.setStyleSheet("font-size:20px;background:transparent;")
        banner_layout.addWidget(shield_icon)
        banner_text = QLabel("【安全硬封锁提示】当前固件处于调试阶段，功率桥臂 PWM 保持硬件封锁，继电器不动作。上位机 START 命令仅用于触发调试状态演进，不输出功率。")
        banner_text.setStyleSheet("color:#f59e0b;font-size:12px;font-weight:500;line-height:1.4;background:transparent;")
        banner_text.setWordWrap(True)
        banner_layout.addWidget(banner_text, 1)

        right_layout.addWidget(banner)

        # 实时快速日志
        log_frame, log_layout = self._panel("实时事件简报")
        self._dash_log = QPlainTextEdit()
        self._dash_log.setObjectName("LogView")
        self._dash_log.setReadOnly(True)
        self._dash_log.setMaximumBlockCount(300)
        log_layout.addWidget(self._dash_log)
        right_layout.addWidget(log_frame, 1)

        layout.addWidget(right_box, 1)
        return container

    # --- 标签 2: 趋势波形 ---
    def _build_trends_tab(self) -> QWidget:
        container = QWidget()
        layout = QVBoxLayout(container)
        layout.setContentsMargins(4, 8, 4, 4)
        layout.setSpacing(8)

        # 曲线控制栏
        ctrl_bar = QHBoxLayout()
        ctrl_bar.setSpacing(12)

        self._chk_iac = QCheckBox("交流电流 Iac (A)")
        self._chk_iac.setChecked(True)
        self._chk_iac.setStyleSheet("color:#2f6fed;font-weight:600;")
        ctrl_bar.addWidget(self._chk_iac)

        self._chk_vdc = QCheckBox("直流电压 Vdc (count)")
        self._chk_vdc.setChecked(True)
        self._chk_vdc.setStyleSheet("color:#f5b83d;font-weight:600;")
        ctrl_bar.addWidget(self._chk_vdc)

        self._chk_vgrid = QCheckBox("电网电压 Vgrid (V)")
        self._chk_vgrid.setChecked(True)
        self._chk_vgrid.setStyleSheet("color:#25c06d;font-weight:600;")
        ctrl_bar.addWidget(self._chk_vgrid)

        self._chk_pll = QCheckBox("PLL 频率 (Hz)")
        self._chk_pll.setChecked(False)
        self._chk_pll.setStyleSheet("color:#a06eed;font-weight:600;")
        ctrl_bar.addWidget(self._chk_pll)

        ctrl_bar.addStretch(1)

        self._btn_pause = QPushButton("⏸ 暂停刷新")
        self._btn_pause.clicked.connect(self._toggle_plot_pause)
        ctrl_bar.addWidget(self._btn_pause)

        self._btn_clear_plot = QPushButton("🗑 清空波形")
        self._btn_clear_plot.clicked.connect(self._clear_plot)
        ctrl_bar.addWidget(self._btn_clear_plot)

        layout.addLayout(ctrl_bar)

        # PyQtGraph 波形图
        self._plot_widget = pg.PlotWidget()
        self._plot_widget.setBackground("#0b1016")
        self._plot_widget.showGrid(x=True, y=True, alpha=0.25)
        self._plot_widget.setLabel("left", "幅值 / 物理量")
        self._plot_widget.setLabel("bottom", "采样点序号")
        self._plot_widget.addLegend(offset=(10, 10))

        self._curve_iac = self._plot_widget.plot(
            pen=pg.mkPen("#2f6fed", width=2), name="Iac (A)"
        )
        self._curve_vdc = self._plot_widget.plot(
            pen=pg.mkPen("#f5b83d", width=2), name="Vdc (count)"
        )
        self._curve_vgrid = self._plot_widget.plot(
            pen=pg.mkPen("#25c06d", width=2), name="Vgrid (V)"
        )
        self._curve_pll = self._plot_widget.plot(
            pen=pg.mkPen("#a06eed", width=2), name="PLL (Hz)"
        )

        layout.addWidget(self._plot_widget, 1)
        return container

    # --- 标签 3: 故障与事件 ---
    def _build_faults_tab(self) -> QWidget:
        container = QWidget()
        layout = QHBoxLayout(container)
        layout.setContentsMargins(4, 8, 4, 4)
        layout.setSpacing(12)

        # 32位故障列表
        left_frame, left_layout = self._panel("CPLD 32 位故障位监控")
        self._fault_table = QTableWidget(16, 3)
        self._fault_table.setHorizontalHeaderLabels(["位", "故障名称", "当前状态"])
        self._fault_table.horizontalHeader().setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        self._fault_table.verticalHeader().setVisible(False)
        self._fault_table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self._fault_table.setSelectionMode(QTableWidget.SelectionMode.NoSelection)
        left_layout.addWidget(self._fault_table)

        self._fault_summary_label = QLabel("当前无活动故障")
        self._fault_summary_label.setStyleSheet("color:#25c06d;font-size:13px;font-weight:600;")
        left_layout.addWidget(self._fault_summary_label)

        layout.addWidget(left_frame, 1)

        # 事件历史日志
        right_frame, right_layout = self._panel("系统事件历史记录")
        self._event_log_view = QPlainTextEdit()
        self._event_log_view.setObjectName("LogView")
        self._event_log_view.setReadOnly(True)
        self._event_log_view.setMaximumBlockCount(1000)
        right_layout.addWidget(self._event_log_view)

        layout.addWidget(right_frame, 1)
        return container

    # --- 标签 4: 通信诊断 ---
    def _build_diag_tab(self) -> QWidget:
        container = QWidget()
        layout = QVBoxLayout(container)
        layout.setContentsMargins(4, 8, 4, 4)
        layout.setSpacing(10)

        # 诊断统计卡片
        stats_layout = QHBoxLayout()
        self._diag_tx = StatCard("发送帧数 TX", "frames")
        self._diag_rx = StatCard("接收帧数 RX", "frames")
        self._diag_crc = StatCard("CRC 错误", "次")
        self._diag_timeout = StatCard("超时次数", "次")
        stats_layout.addWidget(self._diag_tx)
        stats_layout.addWidget(self._diag_rx)
        stats_layout.addWidget(self._diag_crc)
        stats_layout.addWidget(self._diag_timeout)
        layout.addLayout(stats_layout)

        # 原始十六进制收发监视
        frame, diag_layout = self._panel("Modbus RTU 原始报文监控 (最近一次交互)")
        self._tx_hex_edit = QLineEdit()
        self._tx_hex_edit.setReadOnly(True)
        self._tx_hex_edit.setStyleSheet("font-family:Consolas, monospace;background-color:#121820;color:#2f6fed;")
        
        self._rx_hex_edit = QLineEdit()
        self._rx_hex_edit.setReadOnly(True)
        self._rx_hex_edit.setStyleSheet("font-family:Consolas, monospace;background-color:#121820;color:#25c06d;")

        diag_layout.addWidget(QLabel("TX 发送帧:"))
        diag_layout.addWidget(self._tx_hex_edit)
        diag_layout.addWidget(QLabel("RX 响应帧:"))
        diag_layout.addWidget(self._rx_hex_edit)

        # 寄存器原始值表格
        diag_layout.addWidget(QLabel("全部 23 个输入寄存器原始十六进制:"))
        self._reg_table = QTableWidget(1, 23)
        headers = [f"{i:02X}H" for i in range(23)]
        self._reg_table.setHorizontalHeaderLabels(headers)
        self._reg_table.verticalHeader().setVisible(False)
        self._reg_table.setFixedHeight(65)
        self._reg_table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        for c in range(23):
            self._reg_table.setColumnWidth(c, 52)
        diag_layout.addWidget(self._reg_table)

        layout.addWidget(frame, 1)
        return container

    # ------------------------------------------------------------------
    # 底部控制栏
    # ------------------------------------------------------------------
    def _build_bottom_bar(self) -> QWidget:
        footer = QFrame()
        footer.setObjectName("Panel")
        layout = QHBoxLayout(footer)
        layout.setContentsMargins(12, 8, 12, 8)
        layout.setSpacing(14)

        self._uptime_label = QLabel("DSP 运行时间：--")
        self._uptime_label.setStyleSheet("color:#8fa1b3;font-size:13px;")
        layout.addWidget(self._uptime_label)

        self._clock_label = QLabel("")
        self._clock_label.setStyleSheet("color:#6c8299;font-size:13px;")
        layout.addWidget(self._clock_label)

        layout.addStretch(1)

        # CSV 录制
        self._record_btn = QPushButton("🔴 开始 CSV 记录")
        self._record_btn.setMinimumHeight(34)
        self._record_btn.clicked.connect(self._toggle_recording)
        layout.addWidget(self._record_btn)

        self._record_status = QLabel("")
        self._record_status.setStyleSheet("color:#f5b83d;font-size:12px;")
        layout.addWidget(self._record_status)

        layout.addSpacing(16)

        # START 按钮
        self._start_btn = QPushButton("▶ 启动 (START)")
        self._start_btn.setObjectName("StartBtn")
        self._start_btn.setMinimumWidth(130)
        self._start_btn.setMinimumHeight(38)
        self._start_btn.setEnabled(False)
        self._start_btn.clicked.connect(self._on_start_clicked)
        layout.addWidget(self._start_btn)

        # STOP 按钮
        self._stop_btn = QPushButton("■ 停止 (STOP)")
        self._stop_btn.setObjectName("StopBtn")
        self._stop_btn.setMinimumWidth(130)
        self._stop_btn.setMinimumHeight(38)
        self._stop_btn.setEnabled(False)
        self._stop_btn.clicked.connect(self._on_stop_clicked)
        layout.addWidget(self._stop_btn)

        return footer

    # ------------------------------------------------------------------
    # 串口连接与断开
    # ------------------------------------------------------------------
    def _refresh_ports(self):
        current = self._port_combo.currentText()
        self._port_combo.clear()
        ports = []
        if comports is not None:
            try:
                ports = [p.device for p in comports()]
            except Exception:  # noqa: BLE001
                ports = []
        
        # 始终提供虚拟仿真模式供无硬件调试
        ports.append("虚拟仿真 (Simulator)")

        self._port_combo.addItems(ports)
        if current and current in ports:
            self._port_combo.setCurrentText(current)
        else:
            self._port_combo.setCurrentIndex(0)
        self._connect_btn.setEnabled(True)

    def _toggle_connect(self):
        if self._connected:
            self._disconnect()
        else:
            self._connect()

    def _connect(self):
        port = self._port_combo.currentText()
        if not port:
            return
        slave = int(self._slave_spin.currentText())
        baud = int(self._baud_combo.currentText())
        is_sim = "仿真" in port or port == "SIMULATOR"

        self._worker = SerialWorker()
        self._worker.configure(port, slave, baud=baud, timeout=0.1, simulation=is_sim)
        self._worker.reply_ready.connect(self._on_reply)
        self._worker.command_reply.connect(self._on_command_reply)
        self._worker.port_error.connect(self._on_port_error)
        self._worker.connected.connect(self._on_connected)
        self._worker.disconnected.connect(self._on_disconnected)
        self._worker.start()

        self._connect_btn.setEnabled(False)
        self._connect_btn.setText("连接中…")
        mode_str = "【虚拟仿真】" if is_sim else f"【真实串口 {port}】"
        self._log(f"正在连接 {mode_str} @ {baud}，从站地址 {slave}")

    def _disconnect(self):
        if self._worker:
            self._worker.stop()
            self._worker = None
        self._connected = False
        self._update_connection_ui()

    def _on_connected(self):
        self._connected = True
        self._connect_btn.setText("断开")
        self._connect_btn.setEnabled(True)
        self._conn_led.set_on(True)
        self._conn_led.set_text("已连接")
        self._start_btn.setEnabled(True)
        self._stop_btn.setEnabled(True)
        self._log("通信链路建立成功，开始周期轮询。")

    def _on_disconnected(self):
        self._connected = False
        self._connect_btn.setText("连接")
        self._connect_btn.setEnabled(True)
        self._conn_led.set_on(False)
        self._conn_led.set_text("未连接")
        self._start_btn.setEnabled(False)
        self._stop_btn.setEnabled(False)
        self._state_value.setText("已离线")
        self._state_value.setStyleSheet("color:#6c8299;font-size:22px;font-weight:700;")
        self._log("通信链路已断开。")

    def _on_port_error(self, msg: str):
        self._connected = False
        self._connect_btn.setText("连接")
        self._connect_btn.setEnabled(True)
        self._conn_led.set_on(False, "#e5484d")
        self._conn_led.set_text("连接失败")
        self._log(f"串口通信错误：{msg}")

    # ------------------------------------------------------------------
    # 周期轮询与数据处理
    # ------------------------------------------------------------------
    def _on_poll_tick(self):
        if not self._connected or not self._worker:
            return
        self._worker.request_input_block()

    def _on_reply(self, result: FrameResult):
        self._tx_count += 1
        if result.request:
            self._tx_hex_edit.setText(" ".join(f"{b:02X}" for b in result.request))
        if result.response:
            self._rx_hex_edit.setText(" ".join(f"{b:02X}" for b in result.response))
            self._rx_count += 1

        if result.error:
            if "CRC" in result.error:
                self._crc_err_count += 1
            elif "超时" in result.error:
                self._timeout_count += 1
            self._update_diag_stats()
            return

        if not result.extra:
            return

        values = result.extra
        self._latest_raw = list(values)
        try:
            data = rm.parse_input_registers(list(values))
            data["dsp_online"] = True
            self._latest_data = data
        except ValueError as exc:
            self._log(f"数据解析异常：{exc}")
            return

        self._apply_data(data)
        self._update_diag_stats()

        # CSV 实时落盘
        if self._logger.is_recording:
            self._logger.log_record(data, self._latest_raw)
            self._record_status.setText(f"已录制 {self._logger.record_count} 条")

    def _on_command_reply(self, result: FrameResult):
        self._tx_count += 1
        if result.error:
            self._log(f"❌ 命令执行失败：{result.error}")
            return
        if result.extra is not None:
            cmd_val = result.extra
            name = "启动 (START)" if cmd_val == 1 else "停止 (STOP)"
            self._dsp_request_cmd = cmd_val
            self._log(f"✅ 命令写回成功：DSP 请求值已更新为 {name}")
            self._update_cmd_status_box()

    # ------------------------------------------------------------------
    # 数据刷新与界面渲染
    # ------------------------------------------------------------------
    def _apply_data(self, data: dict):
        # 系统状态
        state_text = data["statcom_state"]
        color = state_color(state_text)
        self._state_value.setText(state_text)
        self._state_value.setStyleSheet(f"color:{color};font-size:22px;font-weight:700;")
        self._adc_zero_label.setText(f"ADC 校零状态：{data['adc_zero_state']}")
        self._update_cmd_status_box()

        # CPLD 状态指示灯
        for key, led in self._status_leds.items():
            val = data["cpld_status"].get(key, False)
            if key == "fault_any":
                led.set_on(val, "#e5484d" if val else None)
            else:
                led.set_on(val)

        # 锁相环状态
        self._pll_locked_led.set_on(data["pll_locked"])
        self._pll_valid_led.set_on(data["pll_signal_valid"])

        # 测量卡片
        self._card_grid_v.set_value(f"{data['grid_voltage']:.1f}")
        self._card_iac.set_value(f"{data['iac']:.2f}")
        self._card_vdc.set_value(str(data["cpld_vdc_average"]))
        self._card_pll_f.set_value(f"{data['pll_frequency']:.2f}")
        self._card_temp.set_value(str(data["temperature_count"]))
        self._card_adc_count.set_value(f"{data['adc_sample_count']}")

        # 底部运行时间
        self._uptime_label.setText(f"DSP 运行时间：{format_uptime(data['uptime_ms'])}")

        # 故障表更新
        self._update_fault_table(data)

        # 原始寄存器表格
        if self._latest_raw:
            for i, val in enumerate(self._latest_raw[:23]):
                item = QTableWidgetItem(f"0x{val:04X}")
                item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
                self._reg_table.setItem(0, i, item)

        # 波形追加
        if not self._plot_paused:
            now = time.monotonic()
            self._t_buffer.append(now)
            self._iac_buffer.append(data["iac"])
            self._vdc_buffer.append(float(data["cpld_vdc_average"]))
            self._vgrid_buffer.append(data["grid_voltage"])
            self._pll_buffer.append(data["pll_frequency"])

            xs = list(range(len(self._t_buffer)))
            if self._chk_iac.isChecked():
                self._curve_iac.setData(xs, list(self._iac_buffer))
            else:
                self._curve_iac.clear()

            if self._chk_vdc.isChecked():
                self._curve_vdc.setData(xs, list(self._vdc_buffer))
            else:
                self._curve_vdc.clear()

            if self._chk_vgrid.isChecked():
                self._curve_vgrid.setData(xs, list(self._vgrid_buffer))
            else:
                self._curve_vgrid.clear()

            if self._chk_pll.isChecked():
                self._curve_pll.setData(xs, list(self._pll_buffer))
            else:
                self._curve_pll.clear()

    def _update_cmd_status_box(self):
        if not self._latest_data:
            return
        echo_val = self._latest_data.get("cpld_command_echo_raw", 0)
        dsp_req = "START" if self._dsp_request_cmd == 1 else "STOP"
        echo_str = "START" if echo_val == 1 else "STOP"

        if self._dsp_request_cmd == echo_val:
            status_text = f"请求: {dsp_req} | CPLD回显: {echo_str} (一致)"
            color = "#25c06d" if echo_val == 1 else "#8fa1b3"
        else:
            status_text = f"请求: {dsp_req} | CPLD回显: {echo_str} (等待同步)"
            color = "#f5b83d"
        self._cmd_status_box.setText(status_text)
        self._cmd_status_box.setStyleSheet(f"color:{color};font-size:12px;font-weight:600;")

    def _update_fault_table(self, data: dict):
        fault_map = [
            (0, "bypass_status", "旁路状态异常"),
            (1, "peer_module_fault", "对端模块故障"),
            (4, "dc_overvoltage_fault", "直流硬件过压故障"),
            (5, "drive_fault_1", "驱动 1 硬件故障"),
            (6, "drive_fault_2", "驱动 2 硬件故障"),
            (7, "software_vdc_overvoltage", "软件直流过压告警"),
            (8, "temperature_over", "温度过高频率异常"),
            (10, "uart_frame_error", "UART 帧格式错误"),
            (11, "temperature_sensor_fault", "温度传感器无脉冲故障"),
            (12, "precharge_under", "预充电欠压"),
            (13, "precharge_over", "预充电过压"),
            (14, "precharge_timeout", "预充电超时"),
            (15, "adc_stale", "CPLD ADC 采样超时"),
            (18, "pwm_monitor_fault", "PWM 健康监测故障"),
            (19, "dsp_cpld_link_timeout", "DSP-CPLD 链路超时"),
            (22, "config_invalid", "CPLD 配置参数非法"),
        ]

        active_faults = []
        for row, (bit, key, desc) in enumerate(fault_map):
            is_active = data["cpld_fault"].get(key, False)
            if is_active:
                active_faults.append(desc)

            bit_item = QTableWidgetItem(f"Bit {bit:02d}")
            bit_item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
            desc_item = QTableWidgetItem(desc)
            status_item = QTableWidgetItem("⚠️ 告警触发" if is_active else "正常")
            status_item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)

            if is_active:
                status_item.setForeground(QColor("#e5484d"))
                desc_item.setForeground(QColor("#e5484d"))
            else:
                status_item.setForeground(QColor("#25c06d"))
                desc_item.setForeground(QColor("#8fa1b3"))

            self._fault_table.setItem(row, 0, bit_item)
            self._fault_table.setItem(row, 1, desc_item)
            self._fault_table.setItem(row, 2, status_item)

        if active_faults:
            summary = f"⚠️ 当前活动故障 ({len(active_faults)}项): " + "、".join(active_faults)
            self._fault_summary_label.setText(summary)
            self._fault_summary_label.setStyleSheet("color:#e5484d;font-size:13px;font-weight:600;")
        else:
            self._fault_summary_label.setText("✅ 当前无硬件锁存故障，系统正常。")
            self._fault_summary_label.setStyleSheet("color:#25c06d;font-size:13px;font-weight:600;")

    def _update_diag_stats(self):
        self._diag_tx.set_value(str(self._tx_count))
        self._diag_rx.set_value(str(self._rx_count))
        self._diag_crc.set_value(str(self._crc_err_count))
        self._diag_timeout.set_value(str(self._timeout_count))

    # ------------------------------------------------------------------
    # 控制交互与安全弹窗
    # ------------------------------------------------------------------
    def _on_start_clicked(self):
        if not self._connected or not self._worker:
            self._log("未连接，无法发送启动命令。")
            return

        # 规范 4.4 / 14：START 必须弹出二次确认，明确说明当前为调试命令
        msg_box = QMessageBox(self)
        msg_box.setWindowTitle("⚠️ 启动命令二次确认")
        msg_box.setIcon(QMessageBox.Icon.Warning)
        msg_box.setText(
            "<h3>确认发送 START 调试命令？</h3>"
            "<p><b>安全声明：</b>当前固件处于调试阶段，功率桥臂与继电器处于<b>硬件封锁</b>状态。</p>"
            "<p>发送 START 将触发 DSP/CPLD 调试状态机演进（进入 RUN 状态回显）。</p>"
        )
        msg_box.setStandardButtons(QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.Cancel)
        msg_box.setDefaultButton(QMessageBox.StandardButton.Cancel)

        if msg_box.exec() == QMessageBox.StandardButton.Yes:
            self._worker.queue_write_command(1)
            self._log(">>> [用户操作] 确认发送 START (0x0100=1)")

    def _on_stop_clicked(self):
        if not self._connected or not self._worker:
            self._log("未连接，无法发送停止命令。")
            return
        # STOP 按钮一键直达，不弹窗
        self._worker.queue_write_command(0)
        self._log(">>> [用户操作] 立即发送 STOP (0x0100=0)")

    def _toggle_plot_pause(self):
        self._plot_paused = not self._plot_paused
        self._btn_pause.setText("▶ 继续刷新" if self._plot_paused else "⏸ 暂停刷新")

    def _clear_plot(self):
        self._t_buffer.clear()
        self._iac_buffer.clear()
        self._vdc_buffer.clear()
        self._vgrid_buffer.clear()
        self._pll_buffer.clear()
        self._curve_iac.clear()
        self._curve_vdc.clear()
        self._curve_vgrid.clear()
        self._curve_pll.clear()
        self._log("已清空趋势曲线缓存。")

    def _toggle_recording(self):
        if self._logger.is_recording:
            saved_path = self._logger.stop_recording()
            self._record_btn.setText("🔴 开始 CSV 记录")
            self._record_btn.setStyleSheet("")
            self._record_status.setText(f"已保存: {os.path.basename(saved_path)}")
            self._log(f"CSV 数据记录已停止，文件保存至：{saved_path}")
        else:
            path = self._logger.start_recording()
            self._record_btn.setText("⏹ 停止 CSV 记录")
            self._record_btn.setStyleSheet("background-color:#e5484d;color:#ffffff;")
            self._record_status.setText("正在记录...")
            self._log(f"开始 CSV 数据记录，目标文件：{path}")

    # ------------------------------------------------------------------
    # 工具方法
    # ------------------------------------------------------------------
    def _update_connection_ui(self):
        if self._connected:
            self._connect_btn.setText("断开")
            self._conn_led.set_on(True)
            self._conn_led.set_text("已连接")
        else:
            self._connect_btn.setText("连接")
            self._conn_led.set_on(False)
            self._conn_led.set_text("未连接")

    def _update_clock(self):
        self._clock_label.setText(time.strftime("%Y-%m-%d %H:%M:%S"))

    def _log(self, msg: str):
        ts = time.strftime("%H:%M:%S")
        line = f"[{ts}] {msg}"
        self._dash_log.appendPlainText(line)
        self._event_log_view.appendPlainText(line)

    def closeEvent(self, event: QCloseEvent):
        if self._logger.is_recording:
            self._logger.stop_recording()
        if self._worker:
            self._worker.stop()
            self._worker = None
        super().closeEvent(event)