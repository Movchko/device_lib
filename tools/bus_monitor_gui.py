#!/usr/bin/env python3
"""
bus_monitor_gui.py — GUI для ручной отправки команд BSU и просмотра трафика.

Запуск: python bus_monitor_gui.py
       python bus_monitor_gui.py COM3   # с указанием порта по умолчанию
"""

import sys
import threading
import queue
import time
import socket
from datetime import datetime
from tkinter import (
    Tk, ttk, Frame, Label, Button, Entry, Text, Scrollbar,
    StringVar, BooleanVar, END, DISABLED, NORMAL, BOTH, X, Y, RIGHT, LEFT
)

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("Установите pyserial: pip install pyserial")
    sys.exit(1)

# Импорт протокола из bus_monitor
from bus_monitor import (
    build_can_id,
    build_bsu_can_packet,
    format_packet,
    parse_can_id,
    is_service_packet,
    BSUParser,
    read_config_bytes,
    parse_config_display,
    DEVICE_PPKY_TYPE,
    DEVICE_NAMES,
    SVC_GET_CONFIG_SIZE,
    SVC_GET_CONFIG_CRC,
    SVC_GET_CONFIG_WORD,
    SVC_FIRE_START_EXTINGUISHMENT,
    START_EXT_DELAY_FROM_CMD,
    START_EXT_DELAY_MODULE_ONLY,
    IGNITER_STATUS,
    IGNITER_LINE,
)

SERVICE_CMD_POSITION_DEVICE = 161

WIFI_DEFAULT_HOST = "192.168.4.1"
WIFI_DEFAULT_PORT = "23"


class TcpSerialCompat:
    """Минимальная обертка сокета под интерфейс serial.Serial (read/write/timeout/is_open)."""

    def __init__(self, sock: socket.socket):
        self._sock = sock
        self.timeout = 0.01
        self.is_open = True

    def read(self, size: int = 1) -> bytes:
        if not self.is_open:
            return b""
        try:
            self._sock.settimeout(self.timeout)
            data = self._sock.recv(size)
            if data == b"":
                self.is_open = False
            return data
        except (socket.timeout, BlockingIOError, InterruptedError):
            return b""
        except OSError:
            self.is_open = False
            return b""

    def write(self, data: bytes) -> int:
        if not self.is_open:
            raise OSError("TCP socket is closed")
        self._sock.sendall(data)
        return len(data)

    def reset_input_buffer(self):
        if not self.is_open:
            return
        self._sock.setblocking(False)
        try:
            while True:
                chunk = self._sock.recv(1024)
                if not chunk:
                    break
        except (BlockingIOError, InterruptedError):
            pass
        finally:
            self._sock.setblocking(True)

    def close(self):
        if not self.is_open:
            return
        self.is_open = False
        try:
            self._sock.close()
        except OSError:
            pass


class BusMonitorGUI:
    def __init__(self, default_port: str = ""):
        self.root = Tk()
        self.root.title("BSU Config — ручная отправка команд")
        self.root.minsize(500, 400)
        self.root.geometry("900x620")

        self.ser: serial.Serial | None = None
        self.reader_thread: threading.Thread | None = None
        self.reader_stop = threading.Event()
        self.msg_queue: queue.Queue = queue.Queue()  # str для лога или dict для обновления UI
        self._serial_lock = threading.Lock()
        # Защита от "зависаний" Tk при бурстах трафика: ограничиваем работу UI-потока
        # и не даем потоку чтения крутить CPU на пустых чтениях.
        self._queue_max_items_per_tick = 250
        self._queue_tick_budget_s = 0.015
        self._last_status_refresh_ts = 0.0
        self._status_refresh_interval_s = 0.2
        self._refresh_pending = False
        self._last_igniter_status_ts = 0.0
        self._igniter_status_interval_s = 0.05
        self._rx_silence_reset_s = 2.0
        self._rx_stall_warn_s = 3.0
        self._pps_window_started_at = time.time()
        self._pps_packets_in_window = 0
        self._pps_value = 0
        self.bsu = BSUParser(be_id=False)

        self.can_id_req: int | None = None
        self.can_id_rsp: int | None = None
        self._h_adr_auto_detected = False
        self.h_adr_var = StringVar(value="0")
        self.conn_mode_var = StringVar(value="USB (COM)")
        self.wifi_host_var = StringVar(value=WIFI_DEFAULT_HOST)
        self.wifi_port_var = StringVar(value=WIFI_DEFAULT_PORT)
        self.word_var = StringVar(value="0")
        self.cfg_burst_size_var = StringVar(value="512")
        self.cfg_burst_collect_ms_var = StringVar(value="500")
        self.cfg_burst_rounds_var = StringVar(value="3")
        # Тест «ПОЖАР от МКУ_ТС»: h_adr в CAN всегда 1; зона — индекс как в ППКУ (0…), в ID уходит +1 (0 в ID = все зоны)
        self.mcu_tc_fire_zone_var = StringVar(value="0")
        self.igniter_h_var = StringVar(value="1")
        self.igniter_l_var = StringVar(value="1")
        self.igniter_th_low_var = StringVar(value="1000")
        self.igniter_th_high_var = StringVar(value="3000")
        self.igniter_retry_var = StringVar(value="1")
        self.igniter_start_all_module_delay_var = BooleanVar(value=False)
        self.igniter_status_var = StringVar(value="—")
        self.igniter_sc_check_enabled = True
        self.relay_h_var = StringVar(value="1")
        self.relay_l_var = StringVar(value="1")
        self.relay_mode_options = (
            "0 - нет авто",
            "1 - по пожару",
            "2 - по неисправности",
            "3 - по концевику",
        )
        self.relay_mode_var = StringVar(value=self.relay_mode_options[0])
        self.relay_initial_state_var = StringVar(value="0")
        self.relay_persist_state_var = StringVar(value="0")
        self.button_h_var = StringVar(value="1")
        self.button_l_var = StringVar(value="1")
        self.button_mode_options = (
            "0 - ПУСК СП",
            "1 - Пуск всех зон",
            "2 - Пуск по списку зон",
        )
        self.button_mode_var = StringVar(value=self.button_mode_options[0])
        self.button_zones_var = StringVar(value="1")
        self.button_nc_options = (
            "0 - NO (нормально открытый)",
            "1 - NC (нормально закрытый)",
        )
        self.button_nc_var = StringVar(value=self.button_nc_options[0])
        self.lswitch_h_var = StringVar(value="1")
        self.lswitch_l_var = StringVar(value="1")
        self.lswitch_function_options = (
            "1 - неисправность",
            "2 - ручной режим ППКУ",
            "3 - автоматический режим ППКУ",
            "4 - пауза пуска",
        )
        self.lswitch_function_var = StringVar(value=self.lswitch_function_options[0])
        self.lswitch_trigger_delay_var = StringVar(value="0")
        self.lswitch_nc_options = (
            "0 - NO (нормально открытый)",
            "1 - NC (нормально закрытый)",
        )
        self.lswitch_nc_var = StringVar(value=self.lswitch_nc_options[0])
        self.mcu_zone_type_options = (
            "13 - МКУ_IGN",
            "14 - МКУ_TC",
            "20 - МКУ_K1",
            "21 - МКУ_K2",
            "22 - МКУ_K3",
            "23 - МКУ_KR",
        )
        self.mcu_zone_type_var = StringVar(value=self.mcu_zone_type_options[2])
        self.mcu_zone_h_var = StringVar(value="1")
        self.mcu_zone_value_var = StringVar(value="1")
        self.dpt_emul_enabled_var = BooleanVar(value=False)
        self.dpt_h_var = StringVar(value="1")
        self.dpt_l_var = StringVar(value="1")
        self.new_h_adr_var = StringVar(value="0")
        self.cfg_size_var = StringVar(value="—")
        self.cfg_crc_saved_var = StringVar(value="—")
        self.cfg_crc_local_var = StringVar(value="—")
        self._last_crc_request: str | None = None
        self._config_read_started_at: float | None = None
        self.device_statuses: dict[tuple[int, int, int, int, int], tuple[str, float]] = {}  # key -> (line, last_seen_time)
        # Последние принятые "веса" (ServiceCmd_PositionDevice) по МКУ:
        # key=(d_type,h_adr,l_adr,zone) -> ([w1,w2,...], last_seen_time)
        self.mcu_position_weights: dict[tuple[int, int, int, int], tuple[list[int], float]] = {}
        # Это только UI-таймаут видимости устройств, НЕ таймаут соединения.
        # 15 с давали ложное ощущение "обрыва связи" на нестабильном WiFi.
        self.status_idle_timeout = 60.0  # сек — убирать записи без посылок дольше 60 с

        self._build_ui(default_port)
        self.root.after(50, self._process_queue)
        self.root.after(1000, self._periodic_status_purge)
        self.root.after(1000, self._dpt_emulation_tick)

    def _build_ui(self, default_port: str):
        main = Frame(self.root, padx=10, pady=10)
        main.pack(fill=BOTH, expand=True)

        # --- Строка подключения ---
        conn_frame = Frame(main)
        conn_frame.pack(fill=X, pady=(0, 8))

        Label(conn_frame, text="Режим:").pack(side=LEFT, padx=(0, 4))
        self.conn_mode_combo = ttk.Combobox(
            conn_frame,
            textvariable=self.conn_mode_var,
            values=("USB (COM)", "WiFi (TCP)"),
            state="readonly",
            width=11,
        )
        self.conn_mode_combo.pack(side=LEFT, padx=(0, 8))

        Label(conn_frame, text="Порт:").pack(side=LEFT, padx=(0, 4))
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.port_var = StringVar(value=default_port or (ports[0] if ports else ""))
        self.port_combo = ttk.Combobox(conn_frame, textvariable=self.port_var, width=12)
        self.port_combo["values"] = ports
        self.port_combo.pack(side=LEFT, padx=(0, 8))

        Label(conn_frame, text="IP:").pack(side=LEFT, padx=(8, 2))
        self.wifi_host_entry = Entry(conn_frame, textvariable=self.wifi_host_var, width=12)
        self.wifi_host_entry.pack(side=LEFT, padx=(0, 6))
        Label(conn_frame, text="Port:").pack(side=LEFT, padx=(0, 2))
        self.wifi_port_entry = Entry(conn_frame, textvariable=self.wifi_port_var, width=5)
        self.wifi_port_entry.pack(side=LEFT, padx=(0, 8))

        Label(conn_frame, text="h_adr ППКУ:").pack(side=LEFT, padx=(8, 4))
        Entry(conn_frame, textvariable=self.h_adr_var, width=5).pack(side=LEFT, padx=(0, 8))
        # Кнопка запуска механизма установки адресов в ППКУ (команда 10)
        Button(conn_frame, text="Задать адреса", command=self._send_ppky_auto_address).pack(side=LEFT, padx=(4, 0))
        # Сохранить состояние системы (команда 11)
        Button(conn_frame, text="Сохранить МКУ", command=self._send_ppky_save_system_state).pack(side=LEFT, padx=(8, 0))
        # Применить конфиг-образ ППКУ ко всем МКУ (команда 15)
        Button(conn_frame, text="Применить конфиг", command=self._send_ppky_apply_config_image).pack(side=LEFT, padx=(4, 0))
        # Софт/хард ресет устройств на шине (команда 12, параметр 0/1)
        Button(conn_frame, text="Soft reset", command=self._send_ppky_soft_reset).pack(side=LEFT, padx=(8, 0))
        Button(conn_frame, text="Hard reset", command=self._send_ppky_hard_reset).pack(side=LEFT, padx=(4, 0))

        # Тест: «ПОЖАР» как от МКУ_ТС (h_adr=1 фиксировано; зона — индекс зоны ППКУ, см. Fire_OnStatusFire)
        Label(conn_frame, text="Зона ППКУ:").pack(side=LEFT, padx=(12, 4))
        Entry(conn_frame, textvariable=self.mcu_tc_fire_zone_var, width=4).pack(side=LEFT, padx=(0, 6))
        Button(conn_frame, text="МКУ_ТС: ПОЖАР", command=self._send_mcu_tc_fire).pack(side=LEFT, padx=(4, 0))

        self.connect_btn = Button(conn_frame, text="Подключить", command=self._toggle_connect)
        self.connect_btn.pack(side=LEFT, padx=(8, 0))

        # --- Кнопки команд ---
        cmd_frame = Frame(main)
        cmd_frame.pack(fill=X, pady=(0, 8))

        # GetConfigSize + отображение результата (байт)
        Button(cmd_frame, text="GetConfigSize", command=self._send_get_config_size).pack(side=LEFT, padx=(0, 4))
        Label(cmd_frame, text="size:").pack(side=LEFT, padx=(4, 2))
        self.cfg_size_var = StringVar(value="—")
        Label(cmd_frame, textvariable=self.cfg_size_var, width=8).pack(side=LEFT, padx=(0, 8))

        # CRC сохранённой копии (Saved) и локальной (Local)
        Button(cmd_frame, text="GetConfigCRC (Saved)", command=self._send_get_config_crc_saved).pack(side=LEFT, padx=(4, 4))
        self.cfg_crc_saved_var = StringVar(value="—")
        Label(cmd_frame, textvariable=self.cfg_crc_saved_var, width=10).pack(side=LEFT, padx=(0, 8))

        Button(cmd_frame, text="GetConfigCRC (Local)", command=self._send_get_config_crc_local).pack(side=LEFT, padx=(4, 4))
        self.cfg_crc_local_var = StringVar(value="—")
        Label(cmd_frame, textvariable=self.cfg_crc_local_var, width=10).pack(side=LEFT, padx=(0, 8))

        # Блок чтения слов/полного конфига
        Label(cmd_frame, text="Слово:").pack(side=LEFT, padx=(16, 4))
        Entry(cmd_frame, textvariable=self.word_var, width=6).pack(side=LEFT, padx=(0, 4))
        Button(cmd_frame, text="GetConfigWord", command=self._send_get_config_word).pack(side=LEFT, padx=(0, 8))

        Button(cmd_frame, text="Считать весь конфиг", command=self._read_full_config).pack(side=LEFT, padx=(8, 0))
        Label(cmd_frame, text="burst").pack(side=LEFT, padx=(12, 2))
        Entry(cmd_frame, textvariable=self.cfg_burst_size_var, width=4).pack(side=LEFT, padx=(0, 4))
        Label(cmd_frame, text="collect ms").pack(side=LEFT, padx=(0, 2))
        Entry(cmd_frame, textvariable=self.cfg_burst_collect_ms_var, width=5).pack(side=LEFT, padx=(0, 4))
        Label(cmd_frame, text="rounds").pack(side=LEFT, padx=(0, 2))
        Entry(cmd_frame, textvariable=self.cfg_burst_rounds_var, width=3).pack(side=LEFT, padx=(0, 8))

        # Кнопка установки системного времени ППКУ из времени ПК
        Button(cmd_frame, text="Set PPKY Time (PC)", command=self._send_set_system_time).pack(side=LEFT, padx=(8, 0))

        # --- Панель спички ---
        igniter_frame = Frame(main)
        igniter_frame.pack(fill=X, pady=(0, 8))
        Label(igniter_frame, text="Спичка:").pack(side=LEFT, padx=(0, 4))
        Label(igniter_frame, text="h_adr").pack(side=LEFT, padx=(8, 2))
        Entry(igniter_frame, textvariable=self.igniter_h_var, width=3).pack(side=LEFT, padx=(0, 8))
        Label(igniter_frame, text="l_adr").pack(side=LEFT, padx=(0, 2))
        Entry(igniter_frame, textvariable=self.igniter_l_var, width=3).pack(side=LEFT, padx=(0, 8))
        Button(igniter_frame, text="Запуск", command=self._send_igniter_start).pack(side=LEFT, padx=(8, 0))
        Button(
            igniter_frame,
            text="Пуск всех спичек",
            command=self._send_start_all_igniters,
        ).pack(side=LEFT, padx=(6, 0))
        ttk.Checkbutton(
            igniter_frame,
            text="задержка модуля",
            variable=self.igniter_start_all_module_delay_var,
        ).pack(side=LEFT, padx=(6, 0))
        self.igniter_sc_btn = Button(
            igniter_frame,
            text="Проверка КЗ: ВКЛ",
            command=self._toggle_igniter_sc_check
        )
        self.igniter_sc_btn.pack(side=LEFT, padx=(6, 0))
        Label(igniter_frame, text="low").pack(side=LEFT, padx=(12, 2))
        Entry(igniter_frame, textvariable=self.igniter_th_low_var, width=5).pack(side=LEFT, padx=(0, 4))
        Label(igniter_frame, text="high").pack(side=LEFT, padx=(0, 2))
        Entry(igniter_frame, textvariable=self.igniter_th_high_var, width=5).pack(side=LEFT, padx=(0, 4))
        Label(igniter_frame, text="retry").pack(side=LEFT, padx=(0, 2))
        Entry(igniter_frame, textvariable=self.igniter_retry_var, width=2).pack(side=LEFT, padx=(0, 6))
        Button(igniter_frame, text="Пороги (cmd=12)", command=self._send_igniter_thresholds).pack(side=LEFT, padx=(2, 0))
        Label(igniter_frame, text="Статус:").pack(side=LEFT, padx=(16, 4))
        self.igniter_status_label = Label(igniter_frame, textvariable=self.igniter_status_var, fg="gray")
        self.igniter_status_label.pack(side=LEFT)

        # --- Панель реле ---
        relay_frame = Frame(main)
        relay_frame.pack(fill=X, pady=(0, 8))
        Label(relay_frame, text="Реле:").pack(side=LEFT, padx=(0, 4))
        Label(relay_frame, text="h_adr").pack(side=LEFT, padx=(8, 2))
        Entry(relay_frame, textvariable=self.relay_h_var, width=3).pack(side=LEFT, padx=(0, 8))
        Label(relay_frame, text="l_adr").pack(side=LEFT, padx=(0, 2))
        Entry(relay_frame, textvariable=self.relay_l_var, width=3).pack(side=LEFT, padx=(0, 8))
        Button(relay_frame, text="Вкл", command=self._send_relay_on).pack(side=LEFT, padx=(8, 0))
        Button(relay_frame, text="Выкл", command=self._send_relay_off).pack(side=LEFT, padx=(4, 0))
        Label(relay_frame, text="режим").pack(side=LEFT, padx=(10, 2))
        self.relay_mode_combo = ttk.Combobox(
            relay_frame,
            textvariable=self.relay_mode_var,
            values=self.relay_mode_options,
            state="readonly",
            width=18,
        )
        self.relay_mode_combo.pack(side=LEFT, padx=(0, 4))
        Button(relay_frame, text="Set mode", command=self._send_relay_mode).pack(side=LEFT, padx=(2, 6))
        Label(relay_frame, text="init").pack(side=LEFT, padx=(0, 2))
        self.relay_initial_state_combo = ttk.Combobox(
            relay_frame,
            textvariable=self.relay_initial_state_var,
            values=("0", "1"),
            state="readonly",
            width=2,
        )
        self.relay_initial_state_combo.pack(side=LEFT, padx=(0, 4))
        Button(relay_frame, text="Set init", command=self._send_relay_initial_state).pack(side=LEFT, padx=(2, 6))
        Label(relay_frame, text="persist").pack(side=LEFT, padx=(0, 2))
        self.relay_persist_state_combo = ttk.Combobox(
            relay_frame,
            textvariable=self.relay_persist_state_var,
            values=("0", "1"),
            state="readonly",
            width=2,
        )
        self.relay_persist_state_combo.pack(side=LEFT, padx=(0, 4))
        Button(relay_frame, text="Set persist", command=self._send_relay_persist_state).pack(side=LEFT, padx=(2, 0))

        # --- Панель кнопки ---
        button_frame = Frame(main)
        button_frame.pack(fill=X, pady=(0, 8))
        Label(button_frame, text="Кнопка:").pack(side=LEFT, padx=(0, 4))
        Label(button_frame, text="h_adr").pack(side=LEFT, padx=(8, 2))
        Entry(button_frame, textvariable=self.button_h_var, width=3).pack(side=LEFT, padx=(0, 8))
        Label(button_frame, text="l_adr").pack(side=LEFT, padx=(0, 2))
        Entry(button_frame, textvariable=self.button_l_var, width=3).pack(side=LEFT, padx=(0, 8))
        Label(button_frame, text="режим").pack(side=LEFT, padx=(10, 2))
        self.button_mode_combo = ttk.Combobox(
            button_frame,
            textvariable=self.button_mode_var,
            values=self.button_mode_options,
            state="readonly",
            width=22,
        )
        self.button_mode_combo.pack(side=LEFT, padx=(0, 4))
        Button(button_frame, text="Set mode", command=self._send_button_mode).pack(side=LEFT, padx=(2, 6))
        Label(button_frame, text="зоны").pack(side=LEFT, padx=(0, 2))
        Entry(button_frame, textvariable=self.button_zones_var, width=12).pack(side=LEFT, padx=(0, 4))
        Button(button_frame, text="Set zones", command=self._send_button_zones).pack(side=LEFT, padx=(2, 6))
        Label(button_frame, text="тип").pack(side=LEFT, padx=(0, 2))
        self.button_nc_combo = ttk.Combobox(
            button_frame,
            textvariable=self.button_nc_var,
            values=self.button_nc_options,
            state="readonly",
            width=26,
        )
        self.button_nc_combo.pack(side=LEFT, padx=(0, 4))
        Button(button_frame, text="Set NC", command=self._send_button_normal_closed).pack(side=LEFT, padx=(2, 0))

        # --- Панель концевика ---
        lswitch_frame = Frame(main)
        lswitch_frame.pack(fill=X, pady=(0, 8))
        Label(lswitch_frame, text="Концевик:").pack(side=LEFT, padx=(0, 4))
        Label(lswitch_frame, text="h_adr").pack(side=LEFT, padx=(8, 2))
        Entry(lswitch_frame, textvariable=self.lswitch_h_var, width=3).pack(side=LEFT, padx=(0, 8))
        Label(lswitch_frame, text="l_adr").pack(side=LEFT, padx=(0, 2))
        Entry(lswitch_frame, textvariable=self.lswitch_l_var, width=3).pack(side=LEFT, padx=(0, 8))
        Label(lswitch_frame, text="функция").pack(side=LEFT, padx=(10, 2))
        self.lswitch_function_combo = ttk.Combobox(
            lswitch_frame,
            textvariable=self.lswitch_function_var,
            values=self.lswitch_function_options,
            state="readonly",
            width=28,
        )
        self.lswitch_function_combo.pack(side=LEFT, padx=(0, 4))
        Button(lswitch_frame, text="Set func", command=self._send_lswitch_function).pack(side=LEFT, padx=(2, 6))
        Label(lswitch_frame, text="задержка, с").pack(side=LEFT, padx=(0, 2))
        Entry(lswitch_frame, textvariable=self.lswitch_trigger_delay_var, width=3).pack(side=LEFT, padx=(0, 4))
        Button(lswitch_frame, text="Set delay", command=self._send_lswitch_trigger_delay).pack(side=LEFT, padx=(2, 6))
        Label(lswitch_frame, text="тип").pack(side=LEFT, padx=(0, 2))
        self.lswitch_nc_combo = ttk.Combobox(
            lswitch_frame,
            textvariable=self.lswitch_nc_var,
            values=self.lswitch_nc_options,
            state="readonly",
            width=26,
        )
        self.lswitch_nc_combo.pack(side=LEFT, padx=(0, 4))
        Button(lswitch_frame, text="Set NC", command=self._send_lswitch_normal_closed).pack(side=LEFT, padx=(2, 0))

        # --- Панель назначения зоны МКУ (cmd=20) ---
        mcu_zone_frame = Frame(main)
        mcu_zone_frame.pack(fill=X, pady=(0, 8))
        Label(mcu_zone_frame, text="Зона МКУ (cmd=20):").pack(side=LEFT, padx=(0, 4))
        Label(mcu_zone_frame, text="type").pack(side=LEFT, padx=(8, 2))
        self.mcu_zone_type_combo = ttk.Combobox(
            mcu_zone_frame,
            textvariable=self.mcu_zone_type_var,
            values=self.mcu_zone_type_options,
            state="readonly",
            width=14,
        )
        self.mcu_zone_type_combo.pack(side=LEFT, padx=(0, 8))
        Label(mcu_zone_frame, text="h_adr").pack(side=LEFT, padx=(0, 2))
        Entry(mcu_zone_frame, textvariable=self.mcu_zone_h_var, width=4).pack(side=LEFT, padx=(0, 8))
        Label(mcu_zone_frame, text="zone").pack(side=LEFT, padx=(0, 2))
        Entry(mcu_zone_frame, textvariable=self.mcu_zone_value_var, width=4).pack(side=LEFT, padx=(0, 8))
        Button(mcu_zone_frame, text="Задать зону", command=self._send_mcu_set_zone).pack(side=LEFT, padx=(8, 0))

        # --- Панель эмуляции ДПТ ---
        dpt_frame = Frame(main)
        dpt_frame.pack(fill=X, pady=(0, 8))
        ttk.Checkbutton(
            dpt_frame,
            text="Эмуляция ДПТ (1 раз/сек, статус Норма)",
            variable=self.dpt_emul_enabled_var,
        ).pack(side=LEFT, padx=(0, 8))
        Label(dpt_frame, text="hAdr").pack(side=LEFT, padx=(4, 2))
        Entry(dpt_frame, textvariable=self.dpt_h_var, width=3).pack(side=LEFT, padx=(0, 8))
        Label(dpt_frame, text="lAdr").pack(side=LEFT, padx=(0, 2))
        Entry(dpt_frame, textvariable=self.dpt_l_var, width=3).pack(side=LEFT, padx=(0, 8))
        Button(dpt_frame, text="КЗ", command=self._send_dpt_short_once).pack(side=LEFT, padx=(8, 0))
        Button(dpt_frame, text="ОБРЫВ", command=self._send_dpt_break_once).pack(side=LEFT, padx=(4, 0))

        # --- Панель конфига ---
        config_frame = Frame(main)
        config_frame.pack(fill=X, pady=(0, 4))
        cfg_header = Frame(config_frame)
        cfg_header.pack(fill=X)
        Label(cfg_header, text="Конфигурация (заданные поля):").pack(side=LEFT)
        self.config_debug_var = BooleanVar(value=False)
        ttk.Checkbutton(cfg_header, text="Дамп hex (отладка)", variable=self.config_debug_var).pack(side=LEFT, padx=(16, 0))
        self.config_text = Text(config_frame, wrap="word", font=("Consolas", 9), height=8, state=DISABLED)
        config_scroll = Scrollbar(config_frame, command=self.config_text.yview)
        self.config_text.pack(side=LEFT, fill=X, expand=True)
        config_scroll.pack(side=RIGHT, fill=Y)
        self.config_text.config(yscrollcommand=config_scroll.set)

        # --- Последние статусы устройств ---
        status_header = Frame(main)
        status_header.pack(fill=X)
        Label(status_header, text="Последние статусы устройств:").pack(side=LEFT)
        status_frame = Frame(main)
        status_frame.pack(fill=BOTH, expand=True, pady=(4, 0))
        self.status_text = Text(status_frame, wrap="word", font=("Consolas", 9), height=12, state=DISABLED)
        status_scroll = Scrollbar(status_frame, command=self.status_text.yview)
        self.status_text.pack(side=LEFT, fill=BOTH, expand=True)
        status_scroll.pack(side=RIGHT, fill=Y)
        self.status_text.config(yscrollcommand=status_scroll.set)
        status_bar = Frame(main)
        status_bar.pack(fill=X, pady=(4, 0))
        self.pps_label = Label(status_bar, text="PPS: 0", fg="gray", font=("Consolas", 8), anchor="w")
        self.pps_label.pack(side=LEFT)
        self.log_label = Label(status_bar, text="", fg="gray", font=("Consolas", 8), anchor="w")
        self.log_label.pack(side=LEFT, fill=X, expand=True, padx=(12, 0))

    def _log(self, msg: str, prefix: str = ""):
        """Лог в статусную панель (только для важных событий)."""
        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        self.msg_queue.put({"log": f"[{ts}] {prefix}{msg}"})

    @staticmethod
    def _can_state_letter(state_code: int) -> str:
        """Краткое обозначение состояния CAN-линии: A=active, S=short, B=break."""
        if state_code == 0:
            return "A"
        if state_code == 1:
            return "S"
        if state_code == 2:
            return "B"
        return "?"

    def _append_mcu_can_state(self, base_line: str, parsed: dict, data: bytes) -> str:
        """Добавить декодирование новых статусов CAN для heartbeat МКУ (cmd=0)."""
        if parsed["dir"] != 1:
            return base_line
        if parsed["d_type"] not in (13, 14, 20, 21, 22, 23):
            return base_line
        if len(data) < 8 or data[0] != 0:
            return base_line
        if "C0:" in base_line or "C1:" in base_line:
            return base_line

        can_state_mask = data[7]
        can0 = self._can_state_letter(can_state_mask & 0x03)
        can1 = self._can_state_letter((can_state_mask >> 2) & 0x03)
        return f"{base_line} C0:{can0} C1:{can1}"

    def _update_device_status(self, can_id: int, data: bytes):
        """Обновить последний статус устройства (только dir=1 — ответы от устройств)."""
        p = parse_can_id(can_id)
        if p["dir"] != 1:
            return
        cmd = data[0] if len(data) > 0 else -1
        # Пакет веса МКУ храним отдельно и НЕ даём ему затирать основную строку статуса.
        if (
            cmd == SERVICE_CMD_POSITION_DEVICE
            and p["d_type"] in (13, 14, 20, 21, 22, 23)
            and len(data) >= 2
        ):
            key_w = (p["d_type"], p["h_adr"], p["l_adr"], p["zone"])
            now_ts = time.time()
            prev = self.mcu_position_weights.get(key_w)
            weights = list(prev[0]) if prev else []
            w = int(data[1]) & 0xFF
            if w not in weights:
                weights.append(w)
            else:
                # Если вес уже был, переносим его в конец как "самый свежий".
                weights = [x for x in weights if x != w] + [w]
            # Держим компактный хвост последних наблюдений.
            if len(weights) > 4:
                weights = weights[-4:]
            self.mcu_position_weights[key_w] = (weights, now_ts)
            if not self._refresh_pending:
                self._refresh_pending = True
                self.msg_queue.put({"refresh_status": True})
            return

        # Для ППКУ храним раздельно минимум два типа сообщений:
        # статус (cmd=0) и время (cmd=157), чтобы они не затирали друг друга.
        # Для остальных устройств ключ без разделения по cmd.
        cmd_key = cmd if p["d_type"] == DEVICE_PPKY_TYPE else -1
        key = (p["d_type"], p["h_adr"], p["l_adr"], p["zone"], cmd_key)
        line = format_packet(can_id, data, show_raw_id=False).strip()
        line = self._append_mcu_can_state(line, p, data)
        self.device_statuses[key] = (line, time.time())
        # Коалесинг: ставим обновление статусов в очередь только один раз
        # до фактической перерисовки Tk.
        if not self._refresh_pending:
            self._refresh_pending = True
            self.msg_queue.put({"refresh_status": True})

    def _periodic_status_purge(self):
        """Раз в секунду — удалить устаревшие записи и обновить отображение."""
        now = time.time()
        stale = [k for k, (_, t) in self.device_statuses.items() if now - t > self.status_idle_timeout]
        if stale:
            for k in stale:
                del self.device_statuses[k]
            self.msg_queue.put({"refresh_status": True})
        stale_weights = [k for k, (_, t) in self.mcu_position_weights.items() if now - t > self.status_idle_timeout]
        if stale_weights:
            for k in stale_weights:
                del self.mcu_position_weights[k]
            self.msg_queue.put({"refresh_status": True})
        self.root.after(1000, self._periodic_status_purge)

    def _refresh_status_display(self):
        """Обновить панель статусов устройств. Убирает записи без посылок > 5 с."""
        now = time.time()
        # Удалить записи, по которым не было посылок более 5 с
        stale = [k for k, (_, t) in self.device_statuses.items() if now - t > self.status_idle_timeout]
        for k in stale:
            del self.device_statuses[k]
        stale_weights = [k for k, (_, t) in self.mcu_position_weights.items() if now - t > self.status_idle_timeout]
        for k in stale_weights:
            del self.mcu_position_weights[k]

        self.status_text.config(state=NORMAL)
        self.status_text.delete(1.0, END)
        if not self.device_statuses:
            self.status_text.insert(END, "(нет данных от устройств)")
        else:
            for key in sorted(self.device_statuses.keys()):
                line, last_seen = self.device_statuses[key]
                ts = datetime.fromtimestamp(last_seen).strftime("%H:%M:%S.%f")[:-3]
                dt, ha, la, zn, _cmd_key = key
                if dt in (13, 14, 20, 21, 22, 23):
                    w_key = (dt, ha, la, zn)
                    w_info = self.mcu_position_weights.get(w_key)
                    if w_info and w_info[0]:
                        w_str = ",".join(str(v) for v in w_info[0])
                        line = f"{line} | вес={w_str}"
                self.status_text.insert(END, f"[{ts}] {line}\n")
        self.status_text.config(state=DISABLED)

    def _maybe_auto_h_adr(self, can_id: int):
        """При первом пакете от ППКУ — подставить h_adr в поле."""
        if self._h_adr_auto_detected:
            return
        p = parse_can_id(can_id)
        if p["d_type"] == DEVICE_PPKY_TYPE and p["dir"] == 1:
            self._h_adr_auto_detected = True
            self.msg_queue.put({"h_adr": p["h_adr"]})

    def _maybe_igniter_status(self, can_id: int, data: bytes):
        """При ответе от спички (d_type=11) — обновить статус, если адрес совпадает."""
        p = parse_can_id(can_id)
        if p["d_type"] != 11 or p["dir"] != 1 or len(data) < 3:
            return
        try:
            ih = int(self.igniter_h_var.get() or "1")
            il = int(self.igniter_l_var.get() or "1")
        except ValueError:
            return
        if p["h_adr"] != ih or p["l_adr"] != il:
            return
        now = time.time()
        if now - self._last_igniter_status_ts < self._igniter_status_interval_s:
            return
        self._last_igniter_status_ts = now
        st = IGNITER_STATUS.get(data[0], "?")
        line = IGNITER_LINE.get(data[1], "?")
        flags = data[2]
        start_ack = "✓" if (flags & 0x01) else "—"
        end_ack = "✓" if (flags & 0x02) else "—"
        self.msg_queue.put({"igniter_status": f"{st}, {line} | start_ack={start_ack}, end_ack={end_ack}"})

    def _process_queue(self):
        try:
            refresh = False
            items_processed = 0
            tick_start = time.perf_counter()
            while items_processed < self._queue_max_items_per_tick:
                item = self.msg_queue.get_nowait()
                if isinstance(item, dict):
                    if "h_adr" in item:
                        self.h_adr_var.set(str(item["h_adr"]))
                    elif "igniter_status" in item:
                        self.igniter_status_var.set(item["igniter_status"])
                        self.igniter_status_label.config(fg="black")
                    elif "cfg_size" in item:
                        self.cfg_size_var.set(str(item["cfg_size"]))
                    elif "cfg_crc_saved" in item:
                        crc = item["cfg_crc_saved"]
                        self.cfg_crc_saved_var.set(f"0x{crc:08X}")
                    elif "cfg_crc_local" in item:
                        crc = item["cfg_crc_local"]
                        self.cfg_crc_local_var.set(f"0x{crc:08X}")
                    elif "config_progress" in item:
                        pct, current, total = item["config_progress"]
                        self.config_text.config(state=NORMAL)
                        self.config_text.delete(1.0, END)
                        self.config_text.insert(END, f"Чтение конфигурации: {pct}% ({current}/{total} слов)")
                        self.config_text.config(state=DISABLED)
                    elif "config_result" in item:
                        cfg_bytes, size = item["config_result"]
                        self._apply_config_result(cfg_bytes, size)
                    elif "refresh_status" in item:
                        refresh = True
                    elif "log" in item:
                        if hasattr(self, "log_label"):
                            self.log_label.config(text=item["log"])
                else:
                    pass
                items_processed += 1
                if time.perf_counter() - tick_start > self._queue_tick_budget_s:
                    break
        except queue.Empty:
            pass
        want_refresh = refresh or self._refresh_pending
        if want_refresh and (time.perf_counter() - self._last_status_refresh_ts) >= self._status_refresh_interval_s:
            self._last_status_refresh_ts = time.perf_counter()
            self._refresh_pending = False
            self._refresh_status_display()

        # Обновление счётчика пакетов/с (скользящее окно 1с по входящим кадрам).
        now = time.time()
        if (now - self._pps_window_started_at) >= 1.0:
            self._pps_value = self._pps_packets_in_window
            self._pps_packets_in_window = 0
            self._pps_window_started_at = now
            if hasattr(self, "pps_label"):
                self.pps_label.config(text=f"PPS: {self._pps_value}")
        self.root.after(50, self._process_queue)

    def _update_can_ids(self):
        try:
            h = int(self.h_adr_var.get() or "0")
        except ValueError:
            h = 0
        self.can_id_req = build_can_id(DEVICE_PPKY_TYPE, h, 0, 0, 0)
        self.can_id_rsp = build_can_id(DEVICE_PPKY_TYPE, h, 0, 0, 1)

    def _toggle_connect(self):
        if self.ser and self.ser.is_open:
            self._disconnect()
        else:
            self._connect()

    def _connect(self):
        mode = (self.conn_mode_var.get() or "").strip()
        try:
            if mode == "WiFi (TCP)":
                host = (self.wifi_host_var.get() or "").strip()
                port_txt = (self.wifi_port_var.get() or "").strip()
                if not host:
                    self.msg_queue.put({"log": "[!] Укажите IP WiFi-конвертера"})
                    return
                try:
                    tcp_port = int(port_txt)
                except ValueError:
                    self.msg_queue.put({"log": "[!] Неверный TCP порт"})
                    return
                sock = socket.create_connection((host, tcp_port), timeout=2.0)
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
                self.ser = TcpSerialCompat(sock)
                self.msg_queue.put({"log": f"[*] Подключено по TCP {host}:{tcp_port} (SSID: Pult_bsu4)"})
            else:
                port = self.port_var.get().strip()
                if not port:
                    self.msg_queue.put({"log": "[!] Укажите COM-порт"})
                    return
                # timeout/write_timeout нужны, чтобы при проблемах драйвера или бурсте трафика UI не "зависал" навсегда.
                self.ser = serial.Serial(
                    port,
                    1000000,
                    timeout=0.01,
                    write_timeout=None,
                    xonxoff=False,
                    rtscts=False,
                    dsrdtr=False,
                )
                self.msg_queue.put({"log": f"[*] Подключено к {port}"})

            self._update_can_ids()
            # Новый сеанс — новый парсер, чтобы не тащить состояние после прошлой сессии.
            self.bsu = BSUParser(be_id=False)
            self.reader_stop.clear()
            self.device_statuses.clear()
            self.reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
            self.reader_thread.start()
            self.connect_btn.config(text="Отключить")
            self.conn_mode_combo.config(state=DISABLED)
            self.port_combo.config(state=DISABLED)
            self.wifi_host_entry.config(state=DISABLED)
            self.wifi_port_entry.config(state=DISABLED)
        except (serial.SerialException, OSError, socket.timeout) as e:
            self.msg_queue.put({"log": f"[!] Ошибка: {e}"})

    def _disconnect(self):
        self.reader_stop.set()
        if self.reader_thread:
            self.reader_thread.join(timeout=0.5)
        if self.ser:
            self.ser.close()
            self.ser = None
        self.connect_btn.config(text="Подключить")
        self.conn_mode_combo.config(state="readonly")
        self.port_combo.config(state=NORMAL)
        self.wifi_host_entry.config(state=NORMAL)
        self.wifi_port_entry.config(state=NORMAL)
        self._h_adr_auto_detected = False
        self.dpt_emul_enabled_var.set(False)
        self.msg_queue.put({"log": "[*] Отключено"})

    def _is_wifi_transport(self) -> bool:
        return isinstance(self.ser, TcpSerialCompat) and self.ser.is_open

    def _write_packet(self, pkt: bytes, context: str = "") -> bool:
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return False
        try:
            with self._serial_lock:
                written = self.ser.write(pkt)
                if written is None:
                    written = len(pkt)
                if written < len(pkt):
                    # Нередкий USB кейс: частичная запись при нагрузке драйвера.
                    # Досылаем хвост пакета сразу, не трогая output buffer.
                    rest = pkt[written:]
                    written2 = self.ser.write(rest)
                    if written2 is None:
                        written2 = len(rest)
                    written += written2
                if written < len(pkt):
                    raise OSError(f"partial write: {written}/{len(pkt)} bytes")
            return True
        except Exception as e:
            suffix = f" ({context})" if context else ""
            self.msg_queue.put({"log": f"[!] Write failed{suffix}: {e}"})
            return False

    def _build_dpt_status_packet(self, line_state: int) -> tuple[int, bytes] | None:
        """Сформировать CAN ID и data для статуса ДПТ.
        Формат data: [code, line, R_lo, R_hi, max_tc, max_fault, max_int, reserved].
        """
        try:
            h = int(self.dpt_h_var.get() or "1")
            l = int(self.dpt_l_var.get() or "1")
        except ValueError:
            self.msg_queue.put({"log": "[!] Неверный адрес ДПТ (hAdr/lAdr)"})
            return None

        can_id = build_can_id(12, h, l, 0, 1)  # d_type=12 (ДПТ), dir=1 (как ответ устройства)
        resistance_ohm = 1000  # эмуляция линии "Норма"
        code = 0               # DeviceDPTStatus_Idle
        max_tc = 35
        max_fault = 0
        max_int = 35
        data = bytes([
            code & 0xFF,
            line_state & 0xFF,
            resistance_ohm & 0xFF,
            (resistance_ohm >> 8) & 0xFF,
            max_tc & 0xFF,
            max_fault & 0xFF,
            max_int & 0xFF,
            0,
        ])
        return can_id, data

    def _send_dpt_status(self, line_state: int, label: str):
        if not self.ser or not self.ser.is_open:
            return
        pkt_data = self._build_dpt_status_packet(line_state)
        if pkt_data is None:
            return
        can_id, data = pkt_data
        pkt = build_bsu_can_packet(can_id, data)
        if not self._write_packet(pkt, f"DPT emu {label}"):
            return
        self.msg_queue.put({"log": f">> DPT emu: {label} (h={self.dpt_h_var.get()}, l={self.dpt_l_var.get()})"})

    def _build_mcu_tc_activity_packet(self) -> tuple[int, bytes] | None:
        """Сформировать heartbeat от МКУ_TC (cmd=0), чтобы ППКУ видел активное МКУ."""
        try:
            h = int(self.dpt_h_var.get() or "1")
            l = int(self.dpt_l_var.get() or "1")
        except ValueError:
            self.msg_queue.put({"log": "[!] Неверный адрес МКУ_TC (hAdr/lAdr)"})
            return None

        can_id = build_can_id(14, h, l, 0, 1)  # d_type=14 (МКУ_TC), dir=1
        sec = int(time.time()) & 0xFF
        can_mask = 0x03  # CAN1+CAN2 active
        u24_code_1v = 20
        data = bytes([
            0,  # cmd=0 heartbeat/status
            sec,
            0,
            0,
            0,
            can_mask,
            u24_code_1v,
            0,
        ])
        return can_id, data

    def _send_mcu_tc_activity(self):
        if not self.ser or not self.ser.is_open:
            return
        pkt_data = self._build_mcu_tc_activity_packet()
        if pkt_data is None:
            return
        can_id, data = pkt_data
        pkt = build_bsu_can_packet(can_id, data)
        if not self._write_packet(pkt, "MKU_TC emu"):
            return

    def _send_dpt_short_once(self):
        """Разово отправить статус ДПТ с состоянием линии КЗ."""
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        self._send_dpt_status(2, "КЗ")

    def _send_dpt_break_once(self):
        """Разово отправить статус ДПТ с состоянием линии ОБРЫВ."""
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        self._send_dpt_status(1, "ОБРЫВ")

    def _dpt_emulation_tick(self):
        """Периодическая отправка статуса ДПТ 'Норма' раз в секунду при включённом чекбоксе."""
        try:
            if self.dpt_emul_enabled_var.get():
                self._send_mcu_tc_activity()
                self._send_dpt_status(0, "Норма")
        finally:
            self.root.after(1000, self._dpt_emulation_tick)

    def _reader_loop(self):
        last_rx_ts = time.time()
        rx_stall_reported = False
        while not self.reader_stop.is_set() and self.ser and self.ser.is_open:
            try:
                chunk = self.ser.read(512)
                if not chunk:
                    now = time.time()
                    if (now - last_rx_ts) >= self._rx_stall_warn_s and not rx_stall_reported:
                        rx_stall_reported = True
                        self.msg_queue.put({"log": f"[!] RX тишина {now - last_rx_ts:.1f}с (TCP/WiFi или конвертер)"})
                    # Защита от "залипания" парсера в промежуточном состоянии:
                    # при длительной тишине сбрасываемся в поиск новой преамбулы.
                    if (now - last_rx_ts) >= self._rx_silence_reset_s and getattr(self.bsu, "state", "PREAMBLE_0") != "PREAMBLE_0":
                        self.bsu = BSUParser(be_id=False)
                    time.sleep(0.001)
                    continue
                if rx_stall_reported:
                    gap = time.time() - last_rx_ts
                    self.msg_queue.put({"log": f"[*] RX восстановлен после паузы {gap:.1f}с"})
                    rx_stall_reported = False
                last_rx_ts = time.time()
                for b in chunk:
                    result = self.bsu.feed(b)
                    if result:
                        self._pps_packets_in_window += 1
                        if len(result) >= 2:
                            can_id, data = result[0], result[1]
                        else:
                            continue
                        self._maybe_auto_h_adr(can_id)
                        self._maybe_igniter_status(can_id, data)
                        self._update_device_status(can_id, data)
                        # Обработка сервисных ответов ППКУ (GetConfigSize / GetConfigCRC)
                        if is_service_packet(data):
                            p = parse_can_id(can_id)
                            if p["d_type"] == DEVICE_PPKY_TYPE and p["dir"] == 1:
                                cmd = data[0]
                                if cmd == SVC_GET_CONFIG_SIZE and len(data) >= 5:
                                    size_bytes = ((data[1] << 24) |
                                                  (data[2] << 16) |
                                                  (data[3] << 8)  |
                                                   data[4])
                                    self.msg_queue.put({"cfg_size": size_bytes})
                                elif cmd == SVC_GET_CONFIG_CRC and len(data) >= 5:
                                    crc = (data[1] << 24) | (data[2] << 16) | (data[3] << 8) | data[4]
                                    if self._last_crc_request == "saved":
                                        self.msg_queue.put({"cfg_crc_saved": crc})
                                    elif self._last_crc_request == "local":
                                        self.msg_queue.put({"cfg_crc_local": crc})
            except (serial.SerialException, OSError):
                break
            except Exception as e:
                self.msg_queue.put({"log": f"[!] Reader: {e}"})
        self.reader_stop.clear()

    def _send(self, data: bytes, label: str):
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        self._update_can_ids()
        pkt = build_bsu_can_packet(self.can_id_req, data)
        if not self._write_packet(pkt, label):
            return
        self.msg_queue.put({"log": f">> {label}  data=[{data.hex()}]"})

    def _send_ppky_cmd_broadcast(self, data: bytes, label: str):
        """Отправить команду ППКУ в broadcast-адрес (type=PPKY, h/l=0).
        Это устойчиво к несовпадению текущей зоны/адреса в GUI и фактического runtime-состояния.
        """
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        can_id_broadcast = build_can_id(DEVICE_PPKY_TYPE, 0, 0, 0, 0)
        pkt = build_bsu_can_packet(can_id_broadcast, data)
        if not self._write_packet(pkt, label):
            return
        self.msg_queue.put({"log": f">> {label}  data=[{data.hex()}] (broadcast h=0,l=0)"})

    def _send_ppky_query_with_broadcast_fallback(self, data: bytes, label: str):
        """Отправить запрос ППКУ по выбранному h_adr и дополнительно broadсast (h=0).
        Используется только для безопасных чтений (GetConfig*), чтобы WiFi-режим
        не зависел от корректно введённого/автодетектнутого h_adr.
        """
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return

        self._update_can_ids()
        pkt_main = build_bsu_can_packet(self.can_id_req, data)
        if not self._write_packet(pkt_main, label):
            return

        try:
            selected_h = int(self.h_adr_var.get() or "0")
        except ValueError:
            selected_h = 0

        if selected_h != 0:
            can_id_broadcast = build_can_id(DEVICE_PPKY_TYPE, 0, 0, 0, 0)
            pkt_broadcast = build_bsu_can_packet(can_id_broadcast, data)
            if self._write_packet(pkt_broadcast, f"{label} (broadcast)"):
                self.msg_queue.put({"log": f">> {label}  data=[{data.hex()}] + fallback broadcast(h=0)"})
                return

        self.msg_queue.put({"log": f">> {label}  data=[{data.hex()}]"})

    def _send_circ_set_adr(self):
        """Отправить CircSetAdr с временным отключением ретрансляции:
        1) всем устройствам StopStartReTranslate(1)
        2) CircSetAdr конкретному МКУ
        3) всем устройствам StopStartReTranslate(0)
        """
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        try:
            target = int(self.h_adr_var.get() or "0") & 0xFF
            new_adr = int(self.new_h_adr_var.get() or "0") & 0xFF
        except ValueError:
            self.msg_queue.put({"log": "[!] Неверные h_adr или new_h_adr"})
            return
        # 1) Отключить ретрансляцию: StopStartReTranslate(130), data[1]=1
        can_id_rt = build_can_id(0, 0, 0, 0, 0)  # широковещательно от ППКУ
        data_rt_on = bytes([130, 1]) + b"\x00" * 6
        pkt = build_bsu_can_packet(can_id_rt, data_rt_on)
        if not self._write_packet(pkt, "StopStartReTranslate on"):
            return
        self.msg_queue.put({"log": ">> StopStartReTranslate: 1 (останов ретрансляции)"})

        # Небольшая пауза, чтобы все устройства обработали остановку ретрансляции
        time.sleep(0.5)

        # 2) Команда смены адреса: по кольцу всем МКУ (h_adr=0, l_adr=0)
        can_id = build_can_id(13, 0, 0, 0, 0)  # d_type=13 (МКУ_IGN), dir=0 (запрос), h=0,l=0
        data = bytes([200, new_adr]) + b"\x00" * 6  # cmd=200 CircSetAdr, data[1]=new_adr
        pkt = build_bsu_can_packet(can_id, data)
        if not self._write_packet(pkt, "CircSetAdr"):
            return
        self.msg_queue.put({"log": f">> CircSetAdr: h=0 → new_h_adr={new_adr}"})

        # 3) Включить ретрансляцию обратно: StopStartReTranslate(130), data[1]=0
        data_rt_off = bytes([130, 0]) + b"\x00" * 6
        pkt = build_bsu_can_packet(can_id_rt, data_rt_off)
        if not self._write_packet(pkt, "StopStartReTranslate off"):
            return
        self.msg_queue.put({"log": ">> StopStartReTranslate: 0 (возобновить ретрансляцию)"})

    def _send_get_config_size(self):
        """Запросить размер конфигурации ППКУ (в байтах)."""
        req = bytes([SVC_GET_CONFIG_SIZE]) + b"\x00" * 7
        self._send_ppky_query_with_broadcast_fallback(req, "GetConfigSize")

    def _send_get_config_word(self):
        try:
            i = max(0, int(self.word_var.get() or "0"))
        except ValueError:
            i = 0
        req = bytes([SVC_GET_CONFIG_WORD, (i >> 8) & 0xFF, i & 0xFF]) + b"\x00" * 5
        self._send_ppky_query_with_broadcast_fallback(req, f"GetConfigWord word#{i}")

    def _send_igniter_start(self):
        """Отправить команду «Запуск» (cmd=10) спичке по адресу h_adr, l_adr."""
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        try:
            h = int(self.igniter_h_var.get() or "1")
            l = int(self.igniter_l_var.get() or "1")
        except ValueError:
            self.msg_queue.put({"log": "[!] Неверный адрес спички (h_adr, l_adr)"})
            return
        zone = self._find_active_device_zone_exact(11, h, l)
        used_fallback = False
        if zone is None:
            zone = 0
            used_fallback = True

        can_id = build_can_id(11, h, l, zone, 0)  # d_type=11 (Спичка), dir=0 (запрос)
        data = bytes([10]) + b"\x00" * 7  # cmd=10 — Запуск
        pkt = build_bsu_can_packet(can_id, data)
        if not self._write_packet(pkt, "IgniterStart"):
            return

        if used_fallback:
            self.msg_queue.put({"log": f">> Спичка (h={h}, l={l}) Запуск, zone=0 (fallback: нет свежего статуса)"})
        else:
            self.msg_queue.put({"log": f">> Спичка (h={h}, l={l}, zone={zone}) Запуск"})

    def _send_start_all_igniters(self):
        """Широковещательный пуск всех спичек: StartExtinguishment, zone=0, задержки 0.

        Тип запуска: FROM_CMD (чекбокс снят) или MODULE_ONLY (чекбокс «задержка модуля»).
        """
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return

        zone = 0
        zone_delay = 0
        module_delay = 0
        if self.igniter_start_all_module_delay_var.get():
            launch_type = START_EXT_DELAY_MODULE_ONLY
            type_label = "MODULE_ONLY"
        else:
            launch_type = START_EXT_DELAY_FROM_CMD
            type_label = "FROM_CMD"

        # Broadcast: d_type/h_adr/l_adr/zone=0, dir=0 (как VDeviceButton_SendStartExtinguishment).
        can_id = build_can_id(0, 0, 0, zone, 0)
        data = bytes([
            SVC_FIRE_START_EXTINGUISHMENT,
            zone,
            zone_delay,
            module_delay,
            launch_type,
            0,
            0,
            0,
        ])
        pkt = build_bsu_can_packet(can_id, data)
        if not self._write_packet(pkt, "StartAllIgniters"):
            return
        self.msg_queue.put({
            "log": (
                f">> Пуск всех спичек (broadcast): cmd={SVC_FIRE_START_EXTINGUISHMENT}, "
                f"zone={zone}, z_delay={zone_delay}, m_delay={module_delay}, "
                f"type={launch_type}({type_label})  data=[{data.hex()}]"
            )
        })

    def _toggle_igniter_sc_check(self):
        """Переключить проверку КЗ у спички (cmd=11, val: 0=вкл проверку, 1=выкл)."""
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        try:
            h = int(self.igniter_h_var.get() or "1")
            l = int(self.igniter_l_var.get() or "1")
        except ValueError:
            self.msg_queue.put({"log": "[!] Неверный адрес спички (h_adr, l_adr)"})
            return

        self.igniter_sc_check_enabled = not self.igniter_sc_check_enabled
        # device_igniter.cpp: 0 - проверка КЗ включена, 1 - отключена
        val = 0 if self.igniter_sc_check_enabled else 1

        zone = self._find_active_device_zone_exact(11, h, l)
        if zone is None:
            zone = 0
        can_id = build_can_id(11, h, l, zone, 0)  # d_type=11 (Спичка), dir=0 (запрос)
        data = bytes([11, val]) + b"\x00" * 6
        pkt = build_bsu_can_packet(can_id, data)
        if not self._write_packet(pkt, "IgniterSC"):
            return

        if self.igniter_sc_check_enabled:
            self.igniter_sc_btn.config(text="Проверка КЗ: ВКЛ")
            self.msg_queue.put({"log": f">> Спичка (h={h}, l={l}, zone={zone}) проверка КЗ ВКЛ (cmd=11, val=0)"})
        else:
            self.igniter_sc_btn.config(text="Проверка КЗ: ВЫКЛ")
            self.msg_queue.put({"log": f">> Спичка (h={h}, l={l}, zone={zone}) проверка КЗ ВЫКЛ (cmd=11, val=1)"})

    def _send_igniter_thresholds(self):
        """Задать пороги спички (cmd=12): low/high (LE, мВ), retry (0/1)."""
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        try:
            h = int(self.igniter_h_var.get() or "1")
            l = int(self.igniter_l_var.get() or "1")
            low = int(self.igniter_th_low_var.get() or "0")
            high = int(self.igniter_th_high_var.get() or "0")
            retry = int(self.igniter_retry_var.get() or "0")
        except ValueError:
            self.msg_queue.put({"log": "[!] Неверные поля спички (h/l/low/high/retry)"})
            return

        if h < 0 or h > 255 or l < 0 or l > 63:
            self.msg_queue.put({"log": "[!] Неверный адрес спички: h=0..255, l=0..63"})
            return
        if low < 0 or low > 65535 or high < 0 or high > 65535:
            self.msg_queue.put({"log": "[!] Пороги должны быть в диапазоне 0..65535"})
            return
        retry = 1 if retry > 0 else 0

        zone = self._find_active_device_zone_exact(11, h, l)
        used_fallback = False
        if zone is None:
            zone = 0
            used_fallback = True

        can_id = build_can_id(11, h, l, zone, 0)  # d_type=11 (Спичка), dir=0 (запрос)
        data = bytes([
            12,
            low & 0xFF,
            (low >> 8) & 0xFF,
            high & 0xFF,
            (high >> 8) & 0xFF,
            retry,
            0,
            0,
        ])
        pkt = build_bsu_can_packet(can_id, data)
        if not self._write_packet(pkt, "IgniterSetThresholds"):
            return

        if used_fallback:
            self.msg_queue.put({
                "log": f">> Спичка (h={h}, l={l}) пороги low={low} high={high} retry={retry}, "
                       f"zone=0 (fallback: нет свежего статуса) (cmd=12)"
            })
        else:
            self.msg_queue.put({
                "log": f">> Спичка (h={h}, l={l}, zone={zone}) пороги low={low} high={high} retry={retry} (cmd=12)"
            })

    def _send_relay_set(self, state: int):
        """Отправить команду реле cmd=10 с параметром state (0/1)."""
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        try:
            h = int(self.relay_h_var.get() or "1")
            l = int(self.relay_l_var.get() or "1")
        except ValueError:
            self.msg_queue.put({"log": "[!] Неверный адрес реле (h_adr, l_adr)"})
            return
        zone = self._find_active_device_zone_exact(17, h, l)
        used_fallback = False
        if zone is None:
            zone = 0
            used_fallback = True

        can_id = build_can_id(17, h, l, zone, 0)  # d_type=17 (Реле), dir=0 (запрос)
        data = bytes([10, 1 if state else 0]) + b"\x00" * 6
        pkt = build_bsu_can_packet(can_id, data)
        if not self._write_packet(pkt, "RelaySet"):
            return
        if used_fallback:
            self.msg_queue.put({
                "log": f">> Реле (h={h}, l={l}) {'ВКЛ' if state else 'ВЫКЛ'}, zone=0 "
                       f"(fallback: нет свежего статуса)"
            })
        else:
            self.msg_queue.put({"log": f">> Реле (h={h}, l={l}, zone={zone}) {'ВКЛ' if state else 'ВЫКЛ'}"})

    def _send_relay_on(self):
        self._send_relay_set(1)

    def _send_relay_off(self):
        self._send_relay_set(0)

    def _relay_addr_with_zone(self) -> tuple[int, int, int, bool] | None:
        try:
            h = int(self.relay_h_var.get() or "1")
            l = int(self.relay_l_var.get() or "1")
        except ValueError:
            self.msg_queue.put({"log": "[!] Неверный адрес реле (h_adr, l_adr)"})
            return None
        zone = self._find_active_device_zone_exact(17, h, l)
        used_fallback = False
        if zone is None:
            zone = 0
            used_fallback = True
        return h, l, zone, used_fallback

    def _send_relay_mode(self):
        """Установить режим реле (cmd=11, val=0..3)."""
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        addr = self._relay_addr_with_zone()
        if addr is None:
            return
        h, l, zone, used_fallback = addr
        mode_str = (self.relay_mode_var.get() or "0").strip()
        try:
            mode = int(mode_str.split("-", 1)[0].strip())
        except ValueError:
            mode = 0
        if mode < 0:
            mode = 0
        if mode > 3:
            mode = 3
        can_id = build_can_id(17, h, l, zone, 0)
        data = bytes([11, mode]) + b"\x00" * 6
        pkt = build_bsu_can_packet(can_id, data)
        if not self._write_packet(pkt, "RelaySetMode"):
            return
        if used_fallback:
            self.msg_queue.put({"log": f">> Реле (h={h}, l={l}) mode={mode}, zone=0 (fallback: нет свежего статуса) (cmd=11)"})
        else:
            self.msg_queue.put({"log": f">> Реле (h={h}, l={l}, zone={zone}) mode={mode} (cmd=11)"})

    def _send_relay_initial_state(self):
        """Установить начальное состояние реле (cmd=12, val=0/1)."""
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        addr = self._relay_addr_with_zone()
        if addr is None:
            return
        h, l, zone, used_fallback = addr
        val = 1 if (self.relay_initial_state_var.get() or "0").strip() == "1" else 0
        can_id = build_can_id(17, h, l, zone, 0)
        data = bytes([12, val]) + b"\x00" * 6
        pkt = build_bsu_can_packet(can_id, data)
        if not self._write_packet(pkt, "RelaySetInitial"):
            return
        if used_fallback:
            self.msg_queue.put({"log": f">> Реле (h={h}, l={l}) initial_state={val}, zone=0 (fallback) (cmd=12)"})
        else:
            self.msg_queue.put({"log": f">> Реле (h={h}, l={l}, zone={zone}) initial_state={val} (cmd=12)"})

    def _send_relay_persist_state(self):
        """Установить флаг запоминания состояния реле (cmd=13, val=0/1)."""
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        addr = self._relay_addr_with_zone()
        if addr is None:
            return
        h, l, zone, used_fallback = addr
        val = 1 if (self.relay_persist_state_var.get() or "0").strip() == "1" else 0
        can_id = build_can_id(17, h, l, zone, 0)
        data = bytes([13, val]) + b"\x00" * 6
        pkt = build_bsu_can_packet(can_id, data)
        if not self._write_packet(pkt, "RelaySetPersist"):
            return
        if used_fallback:
            self.msg_queue.put({"log": f">> Реле (h={h}, l={l}) persist={val}, zone=0 (fallback) (cmd=13)"})
        else:
            self.msg_queue.put({"log": f">> Реле (h={h}, l={l}, zone={zone}) persist={val} (cmd=13)"})

    def _button_addr_with_zone(self) -> tuple[int, int, int, bool] | None:
        try:
            h = int(self.button_h_var.get() or "1")
            l = int(self.button_l_var.get() or "1")
        except ValueError:
            self.msg_queue.put({"log": "[!] Неверный адрес кнопки (h_adr, l_adr)"})
            return None
        zone = self._find_active_device_zone_exact(15, h, l)
        used_fallback = False
        if zone is None:
            zone = 0
            used_fallback = True
        return h, l, zone, used_fallback

    def _send_button_mode(self):
        """Установить вид кнопки (cmd=15, val=0..2)."""
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        addr = self._button_addr_with_zone()
        if addr is None:
            return
        h, l, zone, used_fallback = addr
        mode_str = (self.button_mode_var.get() or "0").strip()
        try:
            mode = int(mode_str.split("-", 1)[0].strip())
        except ValueError:
            mode = 0
        if mode < 0:
            mode = 0
        if mode > 2:
            mode = 2
        can_id = build_can_id(15, h, l, zone, 0)
        data = bytes([15, mode]) + b"\x00" * 6
        pkt = build_bsu_can_packet(can_id, data)
        if not self._write_packet(pkt, "ButtonSetMode"):
            return
        mode_label = self.button_mode_options[mode] if mode < len(self.button_mode_options) else str(mode)
        if used_fallback:
            self.msg_queue.put({
                "log": f">> Кнопка (h={h}, l={l}) mode={mode} ({mode_label}), "
                       f"zone=0 (fallback: нет свежего статуса) (cmd=15)"
            })
        else:
            self.msg_queue.put({
                "log": f">> Кнопка (h={h}, l={l}, zone={zone}) mode={mode} ({mode_label}) (cmd=15)"
            })

    def _send_button_zones(self):
        """Установить список зон для режима StartZone (cmd=16, 7 байт)."""
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        addr = self._button_addr_with_zone()
        if addr is None:
            return
        h, l, zone, used_fallback = addr
        zones_raw = (self.button_zones_var.get() or "").replace(";", ",").split(",")
        zones: list[int] = []
        for part in zones_raw:
            part = part.strip()
            if not part:
                continue
            try:
                z = int(part)
            except ValueError:
                self.msg_queue.put({"log": "[!] Зоны: целые числа через запятую (до 7)"})
                return
            if z < 0 or z > 127:
                self.msg_queue.put({"log": "[!] Номер зоны должен быть 0..127"})
                return
            zones.append(z)
        if len(zones) > 7:
            self.msg_queue.put({"log": "[!] Не более 7 зон"})
            return
        zone_bytes = bytes(zones + [0] * (7 - len(zones)))
        can_id = build_can_id(15, h, l, zone, 0)
        data = bytes([16]) + zone_bytes
        pkt = build_bsu_can_packet(can_id, data)
        if not self._write_packet(pkt, "ButtonSetZones"):
            return
        zones_s = ",".join(str(z) for z in zones) if zones else "(пусто)"
        if used_fallback:
            self.msg_queue.put({
                "log": f">> Кнопка (h={h}, l={l}) zones=[{zones_s}], "
                       f"zone=0 (fallback: нет свежего статуса) (cmd=16)"
            })
        else:
            self.msg_queue.put({
                "log": f">> Кнопка (h={h}, l={l}, zone={zone}) zones=[{zones_s}] (cmd=16)"
            })

    def _send_button_normal_closed(self):
        """Установить тип кнопки NO/NC (cmd=17, 0=NO, 1=NC)."""
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        addr = self._button_addr_with_zone()
        if addr is None:
            return
        h, l, zone, used_fallback = addr
        nc_str = (self.button_nc_var.get() or "0").strip()
        try:
            val = int(nc_str.split("-", 1)[0].strip())
        except ValueError:
            val = 0
        val = 1 if val else 0
        can_id = build_can_id(15, h, l, zone, 0)
        data = bytes([17, val]) + b"\x00" * 6
        pkt = build_bsu_can_packet(can_id, data)
        if not self._write_packet(pkt, "ButtonSetNormalClosed"):
            return
        nc_label = self.button_nc_options[val] if val < len(self.button_nc_options) else str(val)
        if used_fallback:
            self.msg_queue.put({
                "log": f">> Кнопка (h={h}, l={l}) тип={nc_label}, "
                       f"zone=0 (fallback: нет свежего статуса) (cmd=17, val={val})"
            })
        else:
            self.msg_queue.put({
                "log": f">> Кнопка (h={h}, l={l}, zone={zone}) тип={nc_label} (cmd=17, val={val})"
            })

    def _lswitch_addr_with_zone(self) -> tuple[int, int, int, bool] | None:
        try:
            h = int(self.lswitch_h_var.get() or "1")
            l = int(self.lswitch_l_var.get() or "1")
        except ValueError:
            self.msg_queue.put({"log": "[!] Неверный адрес концевика (h_adr, l_adr)"})
            return None
        zone = self._find_active_device_zone_exact(16, h, l)
        used_fallback = False
        if zone is None:
            zone = 0
            used_fallback = True
        return h, l, zone, used_fallback

    def _send_lswitch_trigger_delay(self):
        """Установить задержку срабатывания концевика (cmd=15, сек)."""
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        addr = self._lswitch_addr_with_zone()
        if addr is None:
            return
        h, l, zone, used_fallback = addr
        try:
            delay = int(self.lswitch_trigger_delay_var.get() or "0")
        except ValueError:
            self.msg_queue.put({"log": "[!] Задержка: целое число 0..255"})
            return
        if delay < 0 or delay > 255:
            self.msg_queue.put({"log": "[!] Задержка должна быть 0..255 с"})
            return
        can_id = build_can_id(16, h, l, zone, 0)
        data = bytes([15, delay]) + b"\x00" * 6
        pkt = build_bsu_can_packet(can_id, data)
        if not self._write_packet(pkt, "LSwitchSetDelay"):
            return
        if used_fallback:
            self.msg_queue.put({
                "log": f">> Концевик (h={h}, l={l}) trigger_delay={delay}с, "
                       f"zone=0 (fallback: нет свежего статуса) (cmd=15)"
            })
        else:
            self.msg_queue.put({
                "log": f">> Концевик (h={h}, l={l}, zone={zone}) trigger_delay={delay}с (cmd=15)"
            })

    def _send_lswitch_function(self):
        """Установить функцию концевика (cmd=16, val=1..4)."""
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        addr = self._lswitch_addr_with_zone()
        if addr is None:
            return
        h, l, zone, used_fallback = addr
        func_str = (self.lswitch_function_var.get() or "1").strip()
        try:
            func = int(func_str.split("-", 1)[0].strip())
        except ValueError:
            func = 1
        if func < 1:
            func = 1
        if func > 4:
            func = 4
        can_id = build_can_id(16, h, l, zone, 0)
        data = bytes([16, func]) + b"\x00" * 6
        pkt = build_bsu_can_packet(can_id, data)
        if not self._write_packet(pkt, "LSwitchSetFunction"):
            return
        func_label = self.lswitch_function_options[func - 1] if 1 <= func <= len(self.lswitch_function_options) else str(func)
        if used_fallback:
            self.msg_queue.put({
                "log": f">> Концевик (h={h}, l={l}) function={func} ({func_label}), "
                       f"zone=0 (fallback: нет свежего статуса) (cmd=16)"
            })
        else:
            self.msg_queue.put({
                "log": f">> Концевик (h={h}, l={l}, zone={zone}) function={func} ({func_label}) (cmd=16)"
            })

    def _send_lswitch_normal_closed(self):
        """Установить тип концевика NO/NC (cmd=17, 0=NO, 1=NC)."""
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        addr = self._lswitch_addr_with_zone()
        if addr is None:
            return
        h, l, zone, used_fallback = addr
        nc_str = (self.lswitch_nc_var.get() or "0").strip()
        try:
            val = int(nc_str.split("-", 1)[0].strip())
        except ValueError:
            val = 0
        val = 1 if val else 0
        can_id = build_can_id(16, h, l, zone, 0)
        data = bytes([17, val]) + b"\x00" * 6
        pkt = build_bsu_can_packet(can_id, data)
        if not self._write_packet(pkt, "LSwitchSetNormalClosed"):
            return
        nc_label = self.lswitch_nc_options[val] if val < len(self.lswitch_nc_options) else str(val)
        if used_fallback:
            self.msg_queue.put({
                "log": f">> Концевик (h={h}, l={l}) тип={nc_label}, "
                       f"zone=0 (fallback: нет свежего статуса) (cmd=17, val={val})"
            })
        else:
            self.msg_queue.put({
                "log": f">> Концевик (h={h}, l={l}, zone={zone}) тип={nc_label} (cmd=17, val={val})"
            })

    def _find_active_device_addr(self, d_type: int, h_adr: int) -> tuple[int, int] | None:
        """Найти актуальные l_adr и zone по последним активным статусам устройства."""
        now = time.time()
        matches: list[tuple[int, int, float]] = []
        for (dt, ha, la, zn, _cmd_key), (_, last_seen) in self.device_statuses.items():
            if dt != d_type or ha != h_adr:
                continue
            if now - last_seen > self.status_idle_timeout:
                continue
            matches.append((la, zn, last_seen))

        if not matches:
            return None

        # Берем наиболее свежий статус для выбранного type+h_adr.
        matches.sort(key=lambda x: x[2], reverse=True)
        l_adr, zone, _ = matches[0]
        return l_adr, zone

    def _find_active_device_zone_exact(self, d_type: int, h_adr: int, l_adr: int) -> int | None:
        """Найти актуальную zone для точного адреса устройства (type+h_adr+l_adr)."""
        now = time.time()
        matches: list[tuple[int, float]] = []
        for (dt, ha, la, zn, _cmd_key), (_, last_seen) in self.device_statuses.items():
            if dt != d_type or ha != h_adr or la != l_adr:
                continue
            if now - last_seen > self.status_idle_timeout:
                continue
            matches.append((zn, last_seen))

        if not matches:
            return None

        matches.sort(key=lambda x: x[1], reverse=True)
        zone, _ = matches[0]
        return zone

    def _send_mcu_set_zone(self):
        """Задать зону МКУ: cmd=20, data[1]=zone."""
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        try:
            d_type_str = (self.mcu_zone_type_var.get() or "").strip()
            d_type = int(d_type_str.split("-", 1)[0].strip())
            h_adr = int(self.mcu_zone_h_var.get() or "0")
            zone = int(self.mcu_zone_value_var.get() or "0")
        except ValueError:
            self.msg_queue.put({"log": "[!] Неверные поля type/h_adr/zone"})
            return

        if d_type < 0 or d_type > 127:
            self.msg_queue.put({"log": "[!] type должен быть в диапазоне 0..127"})
            return
        if h_adr < 0 or h_adr > 255:
            self.msg_queue.put({"log": "[!] h_adr должен быть в диапазоне 0..255"})
            return
        if zone < 0 or zone > 127:
            self.msg_queue.put({"log": "[!] zone должен быть в диапазоне 0..127"})
            return

        addr = self._find_active_device_addr(d_type, h_adr)
        used_fallback = False
        if addr is None:
            # Fallback: команда назначения зоны должна отправляться даже если в GUI
            # ещё нет свежего статуса устройства (или конвертер подключили позже).
            # Для МКУ используем базовый адрес l_adr=0 и широкую зону=0.
            l_adr, current_zone = 0, 0
            used_fallback = True
        else:
            l_adr, current_zone = addr

        can_id = build_can_id(d_type, h_adr, l_adr, current_zone, 0)
        data = bytes([20, zone]) + b"\x00" * 6
        pkt = build_bsu_can_packet(can_id, data)
        if not self._write_packet(pkt, "SetZone"):
            return

        dev_name = DEVICE_NAMES.get(d_type, f"Type{d_type}")
        if used_fallback:
            self.msg_queue.put({
                "log": f">> {dev_name} (type={d_type}, h={h_adr}) set zone={zone} (cmd=20), "
                       f"fallback addr l=0 zone=0 (нет свежего статуса в GUI)"
            })
        else:
            self.msg_queue.put({
                "log": f">> {dev_name} (type={d_type}, h={h_adr}, l={l_adr}, zone_cur={current_zone}) "
                       f"set zone={zone} (cmd=20)"
            })

    def _send_get_config_crc_saved(self):
        """CRC сохранённой копии конфигурации (SavedCfgptr, MsgData[0]=0)."""
        req = bytes([SVC_GET_CONFIG_CRC, 0]) + b"\x00" * 6
        self._last_crc_request = "saved"
        self._send_ppky_query_with_broadcast_fallback(req, "GetConfigCRC (Saved)")

    def _send_get_config_crc_local(self):
        """CRC локальной копии конфигурации (LocalCfgptr, MsgData[0]=1)."""
        req = bytes([SVC_GET_CONFIG_CRC, 1]) + b"\x00" * 6
        self._last_crc_request = "local"
        self._send_ppky_query_with_broadcast_fallback(req, "GetConfigCRC (Local)")

    def _send_ppky_auto_address(self):
        """Запустить механизм автоматической установки адресов в ППКУ (команда 10).
        Команда отправляется только конкретному ППКУ (DEVICE_PPKY_TYPE) по CAN0.
        h_adr ППКУ берётся из поля (обычно автозаполняется по первым ответам ППКУ)."""
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return

        data = bytes([10]) + b"\x00" * 7  # команда 10, без параметров
        self._send_ppky_cmd_broadcast(data, "PPKY AutoAddress (cmd=10)")

    def _send_ppky_save_system_state(self):
        """Сохранить состояние системы в ППКУ (команда 11): записать найденные МКУ в конфиг ППКУ."""
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        data = bytes([11]) + b"\x00" * 7
        self._send_ppky_cmd_broadcast(data, "PPKY SaveSystemState (cmd=11)")

    def _send_ppky_apply_config_image(self):
        """Применить конфиг-образ из ППКУ ко всем МКУ (команда 15)."""
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        data = bytes([15]) + b"\x00" * 7
        self._send_ppky_cmd_broadcast(data, "PPKY ApplyConfigImage (cmd=15)")

    def _send_ppky_soft_reset(self):
        """Софт‑ресет устройств на шине через ППКУ (команда 12, параметр 0)."""
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        data = bytes([12, 0]) + b"\x00" * 6
        self._send_ppky_cmd_broadcast(data, "PPKY SoftReset (cmd=12, mode=0)")

    def _send_ppky_hard_reset(self):
        """Хард‑ресет устройств на шине через ППКУ (команда 12, параметр 1)."""
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        data = bytes([12, 1]) + b"\x00" * 6
        self._send_ppky_cmd_broadcast(data, "PPKY HardReset (cmd=12, mode=1)")

    def _send_mcu_tc_fire(self):
        """Имитация «ПОЖАР» от МКУ_ТС: h_adr в CAN всегда 1; зона — индекс зоны как в конфиге ППКУ (0…).

        В stm_PPKY Fire_OnStatusFire: поле zone в ID — 0 = все зоны; иначе внутренняя зона = (field − 1).
        Поэтому из GUI отправляем в ID: zone_field = user_index + 1 (для индекса 0…126)."""
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        zone_txt = (self.mcu_tc_fire_zone_var.get() or "0").strip().lower()
        if zone_txt in ("all", "все", "*"):
            z_idx = -1
            zone_in_id = 0
        else:
            try:
                z_idx = int(zone_txt)
            except ValueError:
                self.msg_queue.put({"log": "[!] Зона: число 0…126 или all/все/* (все зоны)"})
                return
            if z_idx < 0 or z_idx > 126:
                self.msg_queue.put({"log": "[!] Зона МКУ_ТС вне диапазона 0…126"})
                return
            zone_in_id = (z_idx + 1) & 0x7F
        h_adr_mcu = 1

        can_id = build_can_id(14, h_adr_mcu, 0, zone_in_id, 1)  # DEVICE_MCU_TC_TYPE=14, dir=1
        data = bytes([140]) + b"\x00" * 7
        pkt = build_bsu_can_packet(can_id, data)
        if not self._write_packet(pkt, "MKU_TC fire"):
            return
        z_log = "все" if zone_in_id == 0 else str(z_idx)
        self.msg_queue.put(
            {"log": f">> МКУ_ТС ПОЖАР: h_adr={h_adr_mcu}, зона_ППКУ={z_log}, поле_zone_в_ID={zone_in_id}, cmd=140"}
        )

    def _send_set_system_time(self):
        """Установить системные время и дату ППКУ из текущих значений ПК.
        Формат данных: BCD HH, BCD MM, BCD SS, BCD YY, BCD MM, BCD DD (ServiceCmd_SetSystemTime=157)."""
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        now = datetime.now()
        hh = now.hour
        mm = now.minute
        ss = now.second
        yy = now.year % 100
        mon = now.month
        day = now.day

        def to_bcd(x: int) -> int:
            return ((x // 10) << 4) | (x % 10)

        bcd_h = to_bcd(hh) & 0xFF
        bcd_m = to_bcd(mm) & 0xFF
        bcd_s = to_bcd(ss) & 0xFF
        bcd_y = to_bcd(yy) & 0xFF
        bcd_mon = to_bcd(mon) & 0xFF
        bcd_day = to_bcd(day) & 0xFF

        # PPKY как Dev 0: d_type=DEVICE_PPKY_TYPE, h_adr из поля, l_adr/zone=0
        try:
            h = int(self.h_adr_var.get() or "0")
        except ValueError:
            h = 0

        can_id = build_can_id(DEVICE_PPKY_TYPE, h, 0, 0, 0)  # dir=0 запрос
        data = bytes([157, bcd_h, bcd_m, bcd_s, bcd_y, bcd_mon, bcd_day])
        self._send_ppky_cmd_broadcast(data, f"SetSystemTime {hh:02d}:{mm:02d}:{ss:02d} {day:02d}.{mon:02d}.{now.year:04d}")

    def _read_full_config(self):
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        try:
            h = int(self.h_adr_var.get() or "0")
        except ValueError:
            h = 0
        try:
            burst_size = int(self.cfg_burst_size_var.get() or "128")
            burst_collect_ms = int(self.cfg_burst_collect_ms_var.get() or "300")
            burst_rounds = int(self.cfg_burst_rounds_var.get() or "3")
        except ValueError:
            self.msg_queue.put({"log": "[!] Параметры burst: целые числа (burst, collect ms, rounds)"})
            return

        if burst_size < 1 or burst_size > 512:
            self.msg_queue.put({"log": "[!] burst должен быть 1..512"})
            return
        if burst_collect_ms < 10 or burst_collect_ms > 5000:
            self.msg_queue.put({"log": "[!] collect ms должен быть 10..5000"})
            return
        if burst_rounds < 1 or burst_rounds > 20:
            self.msg_queue.put({"log": "[!] rounds должен быть 1..20"})
            return

        self.msg_queue.put({
            "log": f"[*] Чтение конфигурации... (burst={burst_size}, collect={burst_collect_ms}ms, rounds={burst_rounds})"
        })
        self._config_read_started_at = time.time()
        self.connect_btn.config(state=DISABLED)
        self.config_text.config(state=NORMAL)
        self.config_text.delete(1.0, END)
        self.config_text.insert(END, "Чтение конфигурации: 0% (0/0 слов)")
        self.config_text.config(state=DISABLED)

        progress_state = {"last_pct": -1, "last_current": -1, "last_ts": 0.0}

        def progress_cb(pct: int, current: int, total: int):
            now = time.time()
            is_done = current >= total if total > 0 else False
            # На WiFi процент долго может оставаться 0, поэтому показываем
            # прогресс и по количеству слов (минимум раз в ~0.4с или каждые 64 слова).
            pct_changed = (pct != progress_state["last_pct"])
            words_advanced = (current - progress_state["last_current"]) >= 64
            timed_flush = (now - progress_state["last_ts"]) >= 0.4
            if not is_done and not (pct_changed or words_advanced or timed_flush):
                return
            progress_state["last_pct"] = pct
            progress_state["last_current"] = current
            progress_state["last_ts"] = now
            self.msg_queue.put({"config_progress": (pct, current, total)})

        def do_read():
            cfg_bytes: bytes | None = None
            size = 0
            try:
                self.reader_stop.set()
                if self.reader_thread:
                    self.reader_thread.join(timeout=1.0)
                # Для USB полезно очистить хвост входного буфера перед серией
                # запросов конфига, чтобы не мешали старые status-пакеты.
                if self.ser and self.ser.is_open and not self._is_wifi_transport():
                    try:
                        self.ser.reset_input_buffer()
                    except Exception:
                        pass
                # Для чтения конфига используем отдельный парсер, чтобы исключить
                # влияние промежуточного состояния основного потока чтения.
                cfg_parser = BSUParser(be_id=False)
                cfg_bytes, size = read_config_bytes(
                    self.ser,
                    cfg_parser,
                    h,
                    progress_callback=progress_cb,
                    word_burst_size=burst_size,
                    word_burst_collect_sec=(burst_collect_ms / 1000.0),
                    word_burst_rounds=burst_rounds,
                    transport_hint=("wifi" if self._is_wifi_transport() else "usb"),
                )
            except Exception as e:
                self.msg_queue.put({"log": f"[!] ReadConfig failed: {e}"})
            finally:
                self.reader_stop.clear()
                if self.ser and self.ser.is_open:
                    self.reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
                    self.reader_thread.start()
                self.msg_queue.put({"config_result": (cfg_bytes, size)})

        threading.Thread(target=do_read, daemon=True).start()

    def _apply_config_result(self, cfg_bytes: bytes | None, size: int):
        elapsed = None
        if self._config_read_started_at is not None:
            elapsed = max(0.0, time.time() - self._config_read_started_at)
        self._config_read_started_at = None
        self.connect_btn.config(state=NORMAL)
        if cfg_bytes is None or size == 0:
            self.msg_queue.put({"log": "[!] Ошибка чтения конфигурации"})
            self.config_text.config(state=NORMAL)
            self.config_text.delete(1.0, END)
            if elapsed is not None:
                self.config_text.insert(END, f"Время чтения: {elapsed:.2f} с\n(ошибка)")
            else:
                self.config_text.insert(END, "(ошибка)")
            self.config_text.config(state=DISABLED)
            return
        if elapsed is not None:
            self.msg_queue.put({"log": f"[*] Конфигурация прочитана: {size} байт за {elapsed:.2f} с"})
        else:
            self.msg_queue.put({"log": f"[*] Конфигурация прочитана: {size} байт"})
        lines = parse_config_display(cfg_bytes, debug_dump=self.config_debug_var.get())
        self.config_text.config(state=NORMAL)
        self.config_text.delete(1.0, END)
        if elapsed is not None:
            self.config_text.insert(END, f"Время чтения: {elapsed:.2f} с\n")
        if lines:
            self.config_text.insert(END, "\n".join(lines))
        else:
            self.config_text.insert(END, "(нет заданных полей)")
        self.config_text.config(state=DISABLED)

    def run(self):
        self.root.mainloop()
        self._disconnect()


def main():
    default_port = sys.argv[1] if len(sys.argv) > 1 else ""
    app = BusMonitorGUI(default_port=default_port)
    app.run()


if __name__ == "__main__":
    main()
