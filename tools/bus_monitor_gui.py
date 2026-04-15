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
    IGNITER_STATUS,
    IGNITER_LINE,
)


class BusMonitorGUI:
    def __init__(self, default_port: str = ""):
        self.root = Tk()
        self.root.title("BSU Config — ручная отправка команд")
        self.root.minsize(500, 400)
        self.root.geometry("700x550")

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
        self.bsu = BSUParser(be_id=False)

        self.can_id_req: int | None = None
        self.can_id_rsp: int | None = None
        self._h_adr_auto_detected = False
        self.h_adr_var = StringVar(value="0")
        self.word_var = StringVar(value="0")
        # Тест «ПОЖАР от МКУ_ТС»: h_adr в CAN всегда 1; зона — индекс как в ППКУ (0…), в ID уходит +1 (0 в ID = все зоны)
        self.mcu_tc_fire_zone_var = StringVar(value="0")
        self.igniter_h_var = StringVar(value="1")
        self.igniter_l_var = StringVar(value="1")
        self.igniter_status_var = StringVar(value="—")
        self.igniter_sc_check_enabled = True
        self.relay_h_var = StringVar(value="1")
        self.relay_l_var = StringVar(value="1")
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
        self.device_statuses: dict[tuple[int, int, int, int], tuple[str, float]] = {}  # key -> (line, last_seen_time)
        self.status_idle_timeout = 15.0  # сек — убирать записи без посылок дольше 15 с

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

        Label(conn_frame, text="Порт:").pack(side=LEFT, padx=(0, 4))
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.port_var = StringVar(value=default_port or (ports[0] if ports else ""))
        self.port_combo = ttk.Combobox(conn_frame, textvariable=self.port_var, width=12)
        self.port_combo["values"] = ports
        self.port_combo.pack(side=LEFT, padx=(0, 8))

        Label(conn_frame, text="h_adr ППКУ:").pack(side=LEFT, padx=(8, 4))
        Entry(conn_frame, textvariable=self.h_adr_var, width=5).pack(side=LEFT, padx=(0, 8))
        # Кнопка запуска механизма установки адресов в ППКУ (команда 10)
        Button(conn_frame, text="Задать адреса", command=self._send_ppky_auto_address).pack(side=LEFT, padx=(4, 0))
        # Сохранить состояние системы (команда 11)
        Button(conn_frame, text="Сохранить МКУ", command=self._send_ppky_save_system_state).pack(side=LEFT, padx=(8, 0))
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
        self.igniter_sc_btn = Button(
            igniter_frame,
            text="Проверка КЗ: ВКЛ",
            command=self._toggle_igniter_sc_check
        )
        self.igniter_sc_btn.pack(side=LEFT, padx=(6, 0))
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
        self.log_label = Label(main, text="", fg="gray", font=("Consolas", 8))
        self.log_label.pack(fill=X, pady=(4, 0))

    def _log(self, msg: str, prefix: str = ""):
        """Лог в статусную панель (только для важных событий)."""
        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        self.msg_queue.put({"log": f"[{ts}] {prefix}{msg}"})

    def _update_device_status(self, can_id: int, data: bytes):
        """Обновить последний статус устройства (только dir=1 — ответы от устройств)."""
        p = parse_can_id(can_id)
        if p["dir"] != 1:
            return
        key = (p["d_type"], p["h_adr"], p["l_adr"], p["zone"])
        line = format_packet(can_id, data, show_raw_id=False).strip()
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
        self.root.after(1000, self._periodic_status_purge)

    def _refresh_status_display(self):
        """Обновить панель статусов устройств. Убирает записи без посылок > 5 с."""
        now = time.time()
        # Удалить записи, по которым не было посылок более 5 с
        stale = [k for k, (_, t) in self.device_statuses.items() if now - t > self.status_idle_timeout]
        for k in stale:
            del self.device_statuses[k]

        self.status_text.config(state=NORMAL)
        self.status_text.delete(1.0, END)
        if not self.device_statuses:
            self.status_text.insert(END, "(нет данных от устройств)")
        else:
            for key in sorted(self.device_statuses.keys()):
                line, last_seen = self.device_statuses[key]
                ts = datetime.fromtimestamp(last_seen).strftime("%H:%M:%S.%f")[:-3]
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
        port = self.port_var.get().strip()
        if not port:
            self.msg_queue.put({"log": "[!] Укажите COM-порт"})
            return
        try:
            # timeout/write_timeout нужны, чтобы при проблемах драйвера или бурсте трафика UI не "зависал" навсегда.
            self.ser = serial.Serial(port, 1000000, timeout=0.01, write_timeout=0.5)
            self._update_can_ids()
            self.reader_stop.clear()
            self.device_statuses.clear()
            self.reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
            self.reader_thread.start()
            self.connect_btn.config(text="Отключить")
            self.port_combo.config(state=DISABLED)
            self.msg_queue.put({"log": f"[*] Подключено к {port}"})
        except serial.SerialException as e:
            self.msg_queue.put({"log": f"[!] Ошибка: {e}"})

    def _disconnect(self):
        self.reader_stop.set()
        if self.reader_thread:
            self.reader_thread.join(timeout=0.5)
        if self.ser:
            self.ser.close()
            self.ser = None
        self.connect_btn.config(text="Подключить")
        self.port_combo.config(state=NORMAL)
        self._h_adr_auto_detected = False
        self.dpt_emul_enabled_var.set(False)
        self.msg_queue.put({"log": "[*] Отключено"})

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
        try:
            with self._serial_lock:
                self.ser.write(pkt)
        except Exception as e:
            self.msg_queue.put({"log": f"[!] DPT emu write failed ({label}): {e}"})
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
        tick = int(time.time() * 1000) & 0xFFFFFFFF
        can_mask = 0x03  # CAN1+CAN2 active
        u24_code_01v = 198  # 19.8V
        data = bytes([
            0,  # cmd=0 heartbeat/status
            tick & 0xFF,
            (tick >> 8) & 0xFF,
            (tick >> 16) & 0xFF,
            (tick >> 24) & 0xFF,
            can_mask,
            u24_code_01v,
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
        try:
            with self._serial_lock:
                self.ser.write(pkt)
        except Exception as e:
            self.msg_queue.put({"log": f"[!] MKU_TC emu write failed: {e}"})
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
        while not self.reader_stop.is_set() and self.ser and self.ser.is_open:
            try:
                chunk = self.ser.read(512)
                if not chunk:
                    time.sleep(0.001)
                    continue
                for b in chunk:
                    result = self.bsu.feed(b)
                    if result:
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
        try:
            with self._serial_lock:
                self.ser.write(pkt)
        except Exception as e:
            self.msg_queue.put({"log": f"[!] Write failed ({label}): {e}"})
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
        self.ser.write(pkt)
        self.msg_queue.put({"log": ">> StopStartReTranslate: 1 (останов ретрансляции)"})

        # Небольшая пауза, чтобы все устройства обработали остановку ретрансляции
        time.sleep(0.5)

        # 2) Команда смены адреса: по кольцу всем МКУ (h_adr=0, l_adr=0)
        can_id = build_can_id(13, 0, 0, 0, 0)  # d_type=13 (МКУ_IGN), dir=0 (запрос), h=0,l=0
        data = bytes([200, new_adr]) + b"\x00" * 6  # cmd=200 CircSetAdr, data[1]=new_adr
        pkt = build_bsu_can_packet(can_id, data)
        self.ser.write(pkt)
        self.msg_queue.put({"log": f">> CircSetAdr: h=0 → new_h_adr={new_adr}"})

        # 3) Включить ретрансляцию обратно: StopStartReTranslate(130), data[1]=0
        data_rt_off = bytes([130, 0]) + b"\x00" * 6
        pkt = build_bsu_can_packet(can_id_rt, data_rt_off)
        self.ser.write(pkt)
        self.msg_queue.put({"log": ">> StopStartReTranslate: 0 (возобновить ретрансляцию)"})

    def _send_get_config_size(self):
        """Запросить размер конфигурации ППКУ (в байтах)."""
        req = bytes([SVC_GET_CONFIG_SIZE]) + b"\x00" * 7
        self._send(req, "GetConfigSize")

    def _send_get_config_word(self):
        try:
            i = max(0, int(self.word_var.get() or "0"))
        except ValueError:
            i = 0
        req = bytes([SVC_GET_CONFIG_WORD, (i >> 8) & 0xFF, i & 0xFF]) + b"\x00" * 5
        self._send(req, f"GetConfigWord word#{i}")

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
        can_id = build_can_id(11, h, l, 0, 0)  # d_type=11 (Спичка), dir=0 (запрос)
        data = bytes([10]) + b"\x00" * 7  # cmd=10 — Запуск
        pkt = build_bsu_can_packet(can_id, data)
        self.ser.write(pkt)
        self.msg_queue.put({"log": f">> Спичка (h={h}, l={l}) Запуск"})

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

        can_id = build_can_id(11, h, l, 0, 0)  # d_type=11 (Спичка), dir=0 (запрос)
        data = bytes([11, val]) + b"\x00" * 6
        pkt = build_bsu_can_packet(can_id, data)
        self.ser.write(pkt)

        if self.igniter_sc_check_enabled:
            self.igniter_sc_btn.config(text="Проверка КЗ: ВКЛ")
            self.msg_queue.put({"log": f">> Спичка (h={h}, l={l}) проверка КЗ ВКЛ (cmd=11, val=0)"})
        else:
            self.igniter_sc_btn.config(text="Проверка КЗ: ВЫКЛ")
            self.msg_queue.put({"log": f">> Спичка (h={h}, l={l}) проверка КЗ ВЫКЛ (cmd=11, val=1)"})

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
        can_id = build_can_id(17, h, l, 0, 0)  # d_type=17 (Реле), dir=0 (запрос)
        data = bytes([10, 1 if state else 0]) + b"\x00" * 6
        pkt = build_bsu_can_packet(can_id, data)
        self.ser.write(pkt)
        self.msg_queue.put({"log": f">> Реле (h={h}, l={l}) {'ВКЛ' if state else 'ВЫКЛ'}"})

    def _send_relay_on(self):
        self._send_relay_set(1)

    def _send_relay_off(self):
        self._send_relay_set(0)

    def _find_active_device_addr(self, d_type: int, h_adr: int) -> tuple[int, int] | None:
        """Найти актуальные l_adr и zone по последним активным статусам устройства."""
        now = time.time()
        matches: list[tuple[int, int, float]] = []
        for (dt, ha, la, zn), (_, last_seen) in self.device_statuses.items():
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
        if addr is None:
            self.msg_queue.put({
                "log": f"[!] Нет активного устройства type={d_type}, h_adr={h_adr}. "
                       f"Дождитесь его статуса на шине."
            })
            return
        l_adr, current_zone = addr

        can_id = build_can_id(d_type, h_adr, l_adr, current_zone, 0)
        data = bytes([20, zone]) + b"\x00" * 6
        pkt = build_bsu_can_packet(can_id, data)
        self.ser.write(pkt)

        dev_name = DEVICE_NAMES.get(d_type, f"Type{d_type}")
        self.msg_queue.put({
            "log": f">> {dev_name} (type={d_type}, h={h_adr}, l={l_adr}, zone_cur={current_zone}) "
                   f"set zone={zone} (cmd=20)"
        })

    def _send_get_config_crc_saved(self):
        """CRC сохранённой копии конфигурации (SavedCfgptr, MsgData[0]=0)."""
        req = bytes([SVC_GET_CONFIG_CRC, 0]) + b"\x00" * 6
        self._last_crc_request = "saved"
        self._send(req, "GetConfigCRC (Saved)")

    def _send_get_config_crc_local(self):
        """CRC локальной копии конфигурации (LocalCfgptr, MsgData[0]=1)."""
        req = bytes([SVC_GET_CONFIG_CRC, 1]) + b"\x00" * 6
        self._last_crc_request = "local"
        self._send(req, "GetConfigCRC (Local)")

    def _send_ppky_auto_address(self):
        """Запустить механизм автоматической установки адресов в ППКУ (команда 10).
        Команда отправляется только конкретному ППКУ (DEVICE_PPKY_TYPE) по CAN0.
        h_adr ППКУ берётся из поля (обычно автозаполняется по первым ответам ППКУ)."""
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return

        # Обновляем can_id_req для PPKY (DEVICE_PPKY_TYPE, текущий h_adr, dir=0)
        self._update_can_ids()

        data = bytes([10]) + b"\x00" * 7  # команда 10, без параметров
        self._send(data, "PPKY AutoAddress (cmd=10)")

    def _send_ppky_save_system_state(self):
        """Сохранить состояние системы в ППКУ (команда 11): записать найденные МКУ в конфиг ППКУ."""
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        self._update_can_ids()
        data = bytes([11]) + b"\x00" * 7
        self._send(data, "PPKY SaveSystemState (cmd=11)")

    def _send_ppky_soft_reset(self):
        """Софт‑ресет устройств на шине через ППКУ (команда 12, параметр 0)."""
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        self._update_can_ids()
        data = bytes([12, 0]) + b"\x00" * 6
        self._send(data, "PPKY SoftReset (cmd=12, mode=0)")

    def _send_ppky_hard_reset(self):
        """Хард‑ресет устройств на шине через ППКУ (команда 12, параметр 1)."""
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        self._update_can_ids()
        data = bytes([12, 1]) + b"\x00" * 6
        self._send(data, "PPKY HardReset (cmd=12, mode=1)")

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
        self.ser.write(pkt)
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
        pkt = build_bsu_can_packet(can_id, data)
        self.ser.write(pkt)
        self.msg_queue.put({"log": f">> SetSystemTime: {hh:02d}:{mm:02d}:{ss:02d} "
                                   f"{day:02d}.{mon:02d}.{now.year:04d} (BCD)"})

    def _read_full_config(self):
        if not self.ser or not self.ser.is_open:
            self.msg_queue.put({"log": "[!] Не подключено"})
            return
        try:
            h = int(self.h_adr_var.get() or "0")
        except ValueError:
            h = 0
        self.msg_queue.put({"log": "[*] Чтение конфигурации..."})
        self.connect_btn.config(state=DISABLED)

        def progress_cb(pct: int, current: int, total: int):
            self.msg_queue.put({"config_progress": (pct, current, total)})

        def do_read():
            self.reader_stop.set()
            if self.reader_thread:
                self.reader_thread.join(timeout=1.0)
            cfg_bytes, size = read_config_bytes(self.ser, self.bsu, h, progress_callback=progress_cb)
            self.reader_stop.clear()
            self.reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
            self.reader_thread.start()
            self.msg_queue.put({"config_result": (cfg_bytes, size)})

        threading.Thread(target=do_read, daemon=True).start()

    def _apply_config_result(self, cfg_bytes: bytes | None, size: int):
        self.connect_btn.config(state=NORMAL)
        if cfg_bytes is None or size == 0:
            self.msg_queue.put({"log": "[!] Ошибка чтения конфигурации"})
            self.config_text.config(state=NORMAL)
            self.config_text.delete(1.0, END)
            self.config_text.insert(END, "(ошибка)")
            self.config_text.config(state=DISABLED)
            return
        self.msg_queue.put({"log": f"[*] Конфигурация прочитана: {size} байт"})
        lines = parse_config_display(cfg_bytes, debug_dump=self.config_debug_var.get())
        self.config_text.config(state=NORMAL)
        self.config_text.delete(1.0, END)
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
