#!/usr/bin/env python3
"""
bus_monitor.py — монитор шины BSU (CAN over USB)

Открывает COM-порт, парсит BSU-пакеты и выводит читаемую информацию.
Запуск: python bus_monitor.py COM3
       python bus_monitor.py COM3 --read-config   # чтение конфигурации с ППКУ
       python bus_monitor.py COM3 --read-config --h-adr 1   # с указанием адреса ППКУ
       python bus_monitor.py --list   # список портов

Сервисные команды (128–130, 150–155, 200):
  — Идут от ПК/ППКУ к устройствам (dir=0, стрелка ←).
  — ResetMCU(128): перезагрузка МКУ.
  — StopStartSend(129): остановка/запуск очереди отправки CAN.
  — GetConfigSize(150), GetConfigWord(152): чтение конфигурации (протокол backend).
"""

import sys
import argparse
import struct
import time
from datetime import datetime

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("Установите pyserial: pip install pyserial")
    sys.exit(1)

# BSU протокол
BSU_PREAMBLE = (0x55, 0xAA)
BSU_HEADER_SIZE = 8
BSU_CAN_PAYLOAD = 12  # 4 id + 8 data
BSU_CHECKSUM_SIZE = 2
BSU_CAN_PKT_SIZE = BSU_HEADER_SIZE + BSU_CAN_PAYLOAD + BSU_CHECKSUM_SIZE
BSU_MAX_PKT_SIZE = 128
BSU_PKT_TYPE_CAN = 0
BSU_PKT_TYPE_CAN2 = 1

# Типы устройств (device_config.h, device.hpp)
DEVICE_NAMES = {
    10: "ППКУ",
    11: "Спичка",
    12: "ДПТ",
    13: "МКУ_IGN",
    14: "МКУ_TC",
    17: "Реле",
    20: "МКУ_K1",
    21: "МКУ_K2",
    22: "МКУ_K3",
    23: "МКУ_KR",
    15: "Кнопка",
    16: "Концевик",
}

# Сервисные команды
SERVICE_CMDS = {
    128: "ResetMCU",
    129: "StopStartSend",
    130: "StopStartReTranslate",
    150: "GetConfigSize",
    151: "GetConfigCRC",
    152: "GetConfigWord",
    153: "SetConfigWord",
    154: "SaveConfig",
    155: "DefaultConfig",
    157: "SetSystemTime",
    200: "CircSetAdr",
}

# Состояния воспламенителя
IGNITER_STATUS = {0: "Idle", 1: "Run", 2: "Err"}
IGNITER_LINE = {0: "Норма", 1: "Обрыв", 2: "КЗ"}

# Состояния ДПТ
DPT_LINE = {0: "Норма", 1: "Обрыв", 2: "КЗ", 3: "Пожар", 4: "Нажатие", 5: "Неисправность"}
BUTTON_LINE = {0: "Норма", 1: "Обрыв", 2: "КЗ", 4: "Нажатие", 5: "Неисправность"}
LSWITCH_LINE = {0: "Норма", 1: "Обрыв", 2: "КЗ", 4: "Открытие", 5: "Неисправность"}
RELAY_POS = {0: "Выключено", 1: "Включено"}
MAX_FAULT_FLAGS = (
    (0x01, "FAULT"),
    (0x02, "SCV"),
    (0x04, "SCG"),
    (0x08, "OC"),
)

# Чтение конфигурации (backend)
DEVICE_PPKY_TYPE = 10
SVC_GET_CONFIG_SIZE = 150
SVC_GET_CONFIG_CRC  = 151
SVC_GET_CONFIG_WORD = 152

# Пожарные сервисные команды (backend.h)
SVC_FIRE_START_EXTINGUISHMENT = 142
START_EXT_DELAY_FROM_CMD = 0
START_EXT_DELAY_MODULE_ONLY = 1
START_EXT_DELAY_ZONE_AND_MODULE = 2


def is_service_packet(data: bytes) -> bool:
    """Пакет считается сервисным, если data[0] — сервисная команда (128–130, 150–155, 200)."""
    return len(data) > 0 and data[0] in SERVICE_CMDS


def build_can_id(d_type: int, h_adr: int, l_adr: int, zone: int, dir_bit: int) -> int:
    """Собрать 29-битный CAN ID. dir_bit: 0=запрос к устройству, 1=ответ от устройства."""
    return (zone & 0x7F) | ((l_adr & 0x3F) << 7) | ((h_adr & 0xFF) << 13) | ((d_type & 0x7F) << 21) | ((dir_bit & 1) << 28)


def build_bsu_can_packet(can_id: int, data: bytes) -> bytes:
    """Собрать BSU-пакет для отправки CAN-фрейма."""
    data = (data + b"\x00" * 8)[:8]
    payload = struct.pack("<I", can_id & 0x1FFFFFFF) + data
    pkt = bytearray()
    pkt.extend(bytes(BSU_PREAMBLE))
    pkt.extend(struct.pack("<H", BSU_CAN_PKT_SIZE))
    pkt.extend(struct.pack("<HH", BSU_PKT_TYPE_CAN, 0))
    pkt.extend(payload)
    crc = bsu_checksum(pkt)
    pkt.extend(struct.pack("<H", crc))
    return bytes(pkt)


def parse_can_id(can_id: int) -> dict:
    """Разбор 29-битного CAN ID (zone:7, l_adr:6, h_adr:8, d_type:7, dir:1)"""
    return {
        "dir": (can_id >> 28) & 1,
        "d_type": (can_id >> 21) & 0x7F,
        "h_adr": (can_id >> 13) & 0xFF,
        "l_adr": (can_id >> 7) & 0x3F,
        "zone": can_id & 0x7F,
    }


def format_device(parsed: dict) -> str:
    name = DEVICE_NAMES.get(parsed["d_type"], f"Type{parsed['d_type']}")
    direction = "→" if parsed["dir"] else "←"
    return f"{name}(h={parsed['h_adr']},l={parsed['l_adr']},z={parsed['zone']}){direction}"


def format_packet(can_id: int, data: bytes, show_raw_id: bool = False, bus_label: str = "") -> str:
    """Форматирование пакета в читаемый вид"""
    parsed = parse_can_id(can_id)
    dev_str = format_device(parsed)
    cmd = data[0] if len(data) > 0 else 0

    # Сервисные команды (128–130, 150–155, 200) — идут от ПК/ППКУ к устройствам (dir=0) или ответы (dir=1)
    if cmd in SERVICE_CMDS:
        cmd_name = SERVICE_CMDS[cmd]
        # Для сервисных команд: если d_type не из 10–14, показываем "ПК→" — возможно иной протокол/порядок байт
        srv_dev = dev_str if parsed["d_type"] in DEVICE_NAMES else "ПК→"
        def _srv_line(s: str) -> str:
            bus = f" [{bus_label}]" if bus_label else ""
            return s + bus + (f"  [ID=0x{can_id:08X}]" if show_raw_id else "")
        if cmd == 128:  # ResetMCU — перезагрузка МКУ
            return _srv_line(f"  {srv_dev} | {cmd_name} (перезагрузка)")
        if cmd == 129:  # StopStartSend — data[1]=0 остановка, 1 запуск очереди CAN
            val = data[1] if len(data) >= 2 else 0
            return _srv_line(f"  {srv_dev} | {cmd_name} ({'старт' if val else 'стоп'} очереди)")
        if cmd == 130:  # StopStartReTranslate
            val = data[1] if len(data) >= 2 else 0
            return _srv_line(f"  {srv_dev} | {cmd_name} ({'вкл' if val else 'выкл'} ретрансляцию)")
        if cmd == 152:  # GetConfigWord — ответ содержит слово
            word_num = (data[1] << 8) | data[2] if len(data) >= 3 else 0
            if parsed["dir"] and len(data) >= 7:
                word = struct.unpack_from(">I", data, 3)[0]
                return _srv_line(f"  {srv_dev} | GetConfigWord word#{word_num} → 0x{word:08X}")
            return _srv_line(f"  {srv_dev} | {cmd_name} word#{word_num}")
        if cmd == 153:  # SetConfigWord
            word_num = (data[1] << 8) | data[2] if len(data) >= 3 else 0
            if parsed["dir"] and len(data) >= 7:
                word = struct.unpack_from(">I", data, 3)[0]
                return _srv_line(f"  {srv_dev} | SetConfigWord word#{word_num} → 0x{word:08X}")
            return _srv_line(f"  {srv_dev} | {cmd_name} word#{word_num}")
        if cmd == 150 and parsed["dir"]:  # GetConfigSize ответ
            size = (data[1] << 8) | data[2] if len(data) >= 3 else 0
            return _srv_line(f"  {srv_dev} | GetConfigSize → {size} байт")
        if cmd == 151 and parsed["dir"]:  # GetConfigCRC ответ
            crc = struct.unpack_from("<I", data, 1)[0] if len(data) >= 5 else 0
            return _srv_line(f"  {srv_dev} | GetConfigCRC → 0x{crc:08X}")
        if cmd == 157 and len(data) >= 7:  # SetSystemTime
            def _bcd_to_int(v: int) -> int:
                return ((v >> 4) & 0x0F) * 10 + (v & 0x0F)

            hh = _bcd_to_int(data[1])
            mm = _bcd_to_int(data[2])
            ss = _bcd_to_int(data[3])
            yy = _bcd_to_int(data[4])
            mon = _bcd_to_int(data[5])
            day = _bcd_to_int(data[6])
            return _srv_line(
                f"  {srv_dev} | SetSystemTime {hh:02d}:{mm:02d}:{ss:02d} {day:02d}.{mon:02d}.20{yy:02d}"
            )
        line = f"  {srv_dev} | {cmd_name}"
        if show_raw_id:
            line += f"  [ID=0x{can_id:08X}]"
        return line

    # Данные по типам устройств
    def _dev_line(s: str) -> str:
        bus = f" [{bus_label}]" if bus_label else ""
        return s + bus + (f"  [ID=0x{can_id:08X}]" if show_raw_id else "")
    if parsed["d_type"] == 10 and parsed["dir"]:  # ППКУ → статус питания
        # Формат статуса ППКУ (через backend, см. AppSetStatus):
        # CAN data[0]   = Code (статус)
        # CAN data[1]   = status_sec_cnt (секунды с запуска, modulo 256)
        # CAN data[2]   = power (канал 4)   — шаг 1 В
        # CAN data[3]   = Rpower (канал 0) — шаг 1 В
        # CAN data[4]   = current1         — шаг 50 мА (code * 0.05 А)
        # CAN data[5]   = current2         — шаг 50 мА
        # CAN data[6]   = internal temp (DTS, int8)
        if len(data) >= 6:
            sec = data[1]
            power_code = data[2]
            rpower_code = data[3]
            cur1_code = data[4]
            cur2_code = data[5]
            u = float(power_code)      # В
            ur = float(rpower_code)    # В
            i1 = cur1_code * 0.05      # А
            i2 = cur2_code * 0.05      # А
            line = f"  {dev_str} | t={sec}s U={u:.0f}V U_res={ur:.0f}V I1={i1:.2f}A I2={i2:.2f}A"
            if len(data) >= 7:
                t_int = data[6] if data[6] < 128 else data[6] - 256
                line += f" T={t_int}°C"
            return _dev_line(line)
        return _dev_line(f"  {dev_str} | PPKY status (len={len(data)})")
    if parsed["d_type"] == 11 and parsed["dir"]:  # Спичка →
        # Формат backend-пакета для статуса Igniter:
        # CAN data[0]   = Code (DeviceIgniterStatus)
        # CAN data[1]   = line_state
        # CAN data[2]   = ack flags (bit0=start_ack, bit1=end_ack)
        # CAN data[3..4]= текущая линия (2 байта, LE)
        if len(data) >= 3:
            st = IGNITER_STATUS.get(data[0], "?")
            line = IGNITER_LINE.get(data[1], "?")
            flags = data[2]
            start_ack = "✓" if (flags & 0x01) else "—"
            end_ack = "✓" if (flags & 0x02) else "—"
            if len(data) >= 5:
                resistance = data[3] | (data[4] << 8)
                return _dev_line(
                    f"  {dev_str} | status={st}, line={line}, R={resistance}Ω, start_ack={start_ack}, end_ack={end_ack}"
                )
            return _dev_line(f"  {dev_str} | status={st}, line={line}, start_ack={start_ack}, end_ack={end_ack}")
    if parsed["d_type"] == 12 and parsed["dir"]:  # ДПТ →
        # Формат backend-пакета для статуса ДПТ:
        # CAN data[0]   = Code (DeviceDPTStatus)
        # CAN data[1]   = состояние линии (LineState)
        # CAN data[2]   = max_fault_mask (битовая маска: FAULT/SCV/SCG/OC)
        # CAN data[3..4]= max_temp_tc_c (°C, int16 LE)
        # CAN data[5..6]= max_temp_internal_c (°C, int16 LE)
        # CAN data[7]   = resistance_x100 (R = data[7] * 100 Ом)
        if len(data) >= 8:
            line_code = data[1]
            line = DPT_LINE.get(line_code, "?")
            max_fault_mask = data[2]
            max_temp_tc = struct.unpack_from("<h", data, 3)[0]
            max_temp_int = struct.unpack_from("<h", data, 5)[0]
            resistance = int(data[7]) * 100
            active_flags = [name for bit, name in MAX_FAULT_FLAGS if (max_fault_mask & bit)]
            flags_str = "|".join(active_flags) if active_flags else "OK"
            return _dev_line(
                f"  {dev_str} | line={line} R={resistance}Ω MAX(tc={max_temp_tc}°C,int={max_temp_int}°C,mask=0x{max_fault_mask:02X}:{flags_str})"
            )
        return _dev_line(f"  {dev_str} | DPT status (len={len(data)})")
    if parsed["d_type"] == 15 and parsed["dir"]:  # Кнопка (на базе ДПТ) →
        # Формат как у DPT: data[1]=LineState, data[2]=fault_mask, data[3..4]=tc(int16), data[5..6]=int(int16), data[7]=R/100
        if len(data) >= 8:
            line_code = data[1]
            line = BUTTON_LINE.get(line_code, f"code{line_code}")
            max_fault_mask = data[2]
            max_temp_tc = struct.unpack_from("<h", data, 3)[0]
            max_temp_int = struct.unpack_from("<h", data, 5)[0]
            resistance = int(data[7]) * 100
            active_flags = [name for bit, name in MAX_FAULT_FLAGS if (max_fault_mask & bit)]
            flags_str = "|".join(active_flags) if active_flags else "OK"
            return _dev_line(
                f"  {dev_str} | line={line} R={resistance}Ω MAX(tc={max_temp_tc}°C,int={max_temp_int}°C,mask=0x{max_fault_mask:02X}:{flags_str})"
            )
        return _dev_line(f"  {dev_str} | Button status (len={len(data)})")
    if parsed["d_type"] == 16 and parsed["dir"]:  # Концевик (на базе ДПТ) →
        if len(data) >= 8:
            line_code = data[1]
            line = LSWITCH_LINE.get(line_code, f"code{line_code}")
            max_fault_mask = data[2]
            max_temp_tc = struct.unpack_from("<h", data, 3)[0]
            max_temp_int = struct.unpack_from("<h", data, 5)[0]
            resistance = int(data[7]) * 100
            active_flags = [name for bit, name in MAX_FAULT_FLAGS if (max_fault_mask & bit)]
            flags_str = "|".join(active_flags) if active_flags else "OK"
            return _dev_line(
                f"  {dev_str} | line={line} R={resistance}Ω MAX(tc={max_temp_tc}°C,int={max_temp_int}°C,mask=0x{max_fault_mask:02X}:{flags_str})"
            )
        return _dev_line(f"  {dev_str} | LSwitch status (len={len(data)})")
    if parsed["d_type"] == 17 and parsed["dir"]:  # Реле →
        # data[1]=actual_state, data[2]=error_flag, data[3]=desired_state
        if len(data) >= 4:
            actual = RELAY_POS.get(data[1], f"state{data[1]}")
            err = "Ошибка" if data[2] else "ОК"
            desired = RELAY_POS.get(data[3], f"state{data[3]}")
            return _dev_line(f"  {dev_str} | pos={actual} expected={desired} {err}")
        return _dev_line(f"  {dev_str} | Relay status (len={len(data)})")
    if parsed["d_type"] in (13, 14, 20, 21, 22, 23) and parsed["dir"]:
        # МКУ heartbeat (cmd=0) в разных версиях прошивки:
        #   новый:  [1]=sec, [2]=DTS temp (int8), [3..4]=0, [5]=CAN active, [6]=U24(1V), [7]=CAN state
        #   legacy: [1..4]=tick32,        [5]=CAN active, [6]=0,      [7]=0
        if len(data) >= 6 and data[0] == 0:
            can_flags = int(data[5])
            can1 = "✓" if (can_flags & 0x01) else "—"
            can2 = "✓" if (can_flags & 0x02) else "—"

            is_new_layout = (len(data) >= 6 and data[3] == 0 and data[4] == 0)
            if is_new_layout:
                sec = int(data[1])
                parts = [f"t={sec}s", f"CAN1={can1}", f"CAN2={can2}"]
                if len(data) >= 3:
                    t_int = data[2] if data[2] < 128 else data[2] - 256
                    parts.append(f"T={t_int}°C")
                if len(data) >= 7:
                    parts.append(f"U24={int(data[6]):.0f}V")
            else:
                tick_ms = int(data[1]) | (int(data[2]) << 8) | (int(data[3]) << 16) | (int(data[4]) << 24)
                parts = [f"tick={tick_ms}ms", f"CAN1={can1}", f"CAN2={can2}"]

            if len(data) >= 8:
                can_state_mask = int(data[7])
                s0 = can_state_mask & 0x03
                s1 = (can_state_mask >> 2) & 0x03
                st_map = {0: "A", 1: "S", 2: "B"}
                parts.append(f"C0={st_map.get(s0, '?')}")
                parts.append(f"C1={st_map.get(s1, '?')}")

            return _dev_line(f"  {dev_str} | " + " ".join(parts))
        return _dev_line(f"  {dev_str} | heartbeat")

    # Обычный пакет
    hex_data = " ".join(f"{b:02X}" for b in data[:8])
    bus = f" [{bus_label}]" if bus_label else ""
    line = f"  {dev_str} | cmd={cmd} data=[{hex_data}]{bus}"
    if show_raw_id:
        line += f"  [ID=0x{can_id:08X}]"
    return line


def bsu_checksum(data: bytes) -> int:
    return sum(data) & 0xFFFF


class BSUParser:
    def __init__(self, be_id: bool = False):
        self.be_id = be_id
        self.state = "PREAMBLE_0"
        self.buf = bytearray()
        self.size = 0
        self.type_val = 0
        self.bus_label = ""
        self.total = 0
        self.checksum_acc = 0
        self.crc_lo = 0

    def feed(self, b: int) -> tuple[int, bytes, str] | None:
        """Принять байт, вернуть (can_id, data, bus_label) при полном пакете или None"""
        if self.state == "PREAMBLE_0":
            if b == BSU_PREAMBLE[0]:
                self.state = "PREAMBLE_1"
            return None

        if self.state == "PREAMBLE_1":
            if b == BSU_PREAMBLE[1]:
                self.state = "SIZE_LO"
                self.checksum_acc = BSU_PREAMBLE[0] + BSU_PREAMBLE[1]
            else:
                self.state = "PREAMBLE_0"
            return None

        if self.state == "SIZE_LO":
            self.size = b
            self.checksum_acc += b
            self.state = "SIZE_HI"
            return None

        if self.state == "SIZE_HI":
            self.size |= b << 8
            self.checksum_acc += b
            # Ограничение размера: не принимаем заведомо некорректные/битые длины.
            # При size > 128 сразу возвращаемся к поиску преамбулы.
            if self.size < (BSU_HEADER_SIZE + BSU_CHECKSUM_SIZE) or self.size > BSU_MAX_PKT_SIZE:
                self.state = "PREAMBLE_0"
                return None
            self.state = "TYPE_LO"
            return None

        if self.state == "TYPE_LO":
            self.type_val = b
            self.checksum_acc += b
            self.state = "TYPE_HI"
            return None

        if self.state == "TYPE_HI":
            self.type_val |= b << 8
            self.checksum_acc += b
            self.state = "SEQ_LO"
            return None

        if self.state == "SEQ_LO":
            self.checksum_acc += b
            self.state = "SEQ_HI"
            return None

        if self.state == "SEQ_HI":
            self.checksum_acc += b
            self.total = self.size - BSU_HEADER_SIZE - BSU_CHECKSUM_SIZE
            # Защита от рассинхронизации потока:
            # принимаем только кадр фиксированного размера BSU_CAN_PKT_SIZE (22 байта).
            # Иначе один битый байт в size/type может "залипнуть" парсер в BODY
            # на сотни байт и визуально дать паузу RX на секунды.
            if (
                self.size != BSU_CAN_PKT_SIZE
                or self.total != BSU_CAN_PAYLOAD
                or self.type_val not in (BSU_PKT_TYPE_CAN, BSU_PKT_TYPE_CAN2)
            ):
                self.state = "PREAMBLE_0"
                return None
            self.bus_label = "CAN2" if self.type_val == BSU_PKT_TYPE_CAN2 else "CAN1"
            self.state = "BODY"
            self.buf = bytearray()
            return None

        if self.state == "BODY":
            self.buf.append(b)
            self.checksum_acc += b
            if len(self.buf) >= self.total:
                self.state = "CRC_LO"
            return None

        if self.state == "CRC_LO":
            self.crc_lo = b
            self.state = "CRC_HI"
            return None

        if self.state == "CRC_HI":
            recv_crc = self.crc_lo | (b << 8)
            calc_crc = self.checksum_acc & 0xFFFF
            self.state = "PREAMBLE_0"
            if calc_crc == recv_crc and len(self.buf) >= 12:
                can_id = struct.unpack(">I" if self.be_id else "<I", self.buf[:4])[0] & 0x1FFFFFFF
                data = bytes(self.buf[4:12])
                return (can_id, data, self.bus_label)
            return None

        self.state = "PREAMBLE_0"
        return None


def run_read_config(ser, bsu: BSUParser, h_adr: int | None, l_adr: int = 0, zone: int = 0, quiet: bool = False, debug: bool = False, full_log: bool = False) -> int:
    """
    Режим чтения конфигурации с ППКУ.
    Протокол: GetConfigSize(150) → размер, затем цикл GetConfigWord(152) по индексу слова.
    Возвращает 0 при успехе, -1 при ошибке.
    """
    d_type = DEVICE_PPKY_TYPE
    can_id_req = build_can_id(d_type, h_adr or 0, l_adr, zone, 0)
    can_id_rsp = build_can_id(d_type, h_adr or 0, l_adr, zone, 1)

    def send_req(data: bytes) -> None:
        pkt = build_bsu_can_packet(can_id_req, data)
        ser.write(pkt)

    RETRY_TIMEOUT_MS = 0.005   # 5 мс — если нет ответа, перезапрос
    TOTAL_TIMEOUT_SEC = 2.0    # общий таймаут на один запрос

    def wait_response(req_data: bytes, expected_cmd: int, expected_word_idx: int | None = None, req_label: str = "") -> bytes | None:
        """Ждёт ответ. Если за 5 мс нет ответа — перезапрос. Для GetConfigWord — expected_word_idx."""
        old_timeout = ser.timeout
        ser.timeout = 0  # non-blocking
        deadline = time.time() + TOTAL_TIMEOUT_SEC
        pkt_count = 0
        found = False
        retry_count = 0
        log_pkts = full_log or debug
        try:
            while time.time() < deadline:
                send_req(req_data)
                retry_count += 1
                if retry_count > 1:
                    ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
                    if retry_count <= 10 or retry_count % 10 == 0:
                        print(f">> RETRY [{ts}] {req_label} (попытка #{retry_count})")
                retry_deadline = time.time() + RETRY_TIMEOUT_MS
                while time.time() < retry_deadline:
                    chunk = ser.read(512)
                    for b in chunk:
                        result = bsu.feed(b)
                        if result:
                            rid = result[0]
                            rdata = result[1]
                            pkt_count += 1
                            if log_pkts:
                                ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
                                p = parse_can_id(rid)
                                match = " ✓" if (rid == can_id_rsp and len(rdata) > 0 and rdata[0] == expected_cmd and
                                                (expected_word_idx is None or (len(rdata) >= 3 and (rdata[1] << 8) | rdata[2] == expected_word_idx))) else ""
                                print(f"<< PKT [{ts}] ID=0x{rid:08X} d_type={p['d_type']} dir={p['dir']} data=[{rdata.hex()}]{match}")
                            if rid == can_id_rsp and len(rdata) > 0 and rdata[0] == expected_cmd:
                                if expected_word_idx is not None and len(rdata) >= 3:
                                    got_idx = (rdata[1] << 8) | rdata[2]
                                    if got_idx != expected_word_idx:
                                        continue
                                found = True
                                return rdata
                    if not chunk:
                        pass  # временно без sleep
        finally:
            ser.timeout = old_timeout
            if log_pkts and not found:
                if pkt_count == 0:
                    print(f"<< Таймаут после {retry_count} перезапросов: пакетов не получено")
                else:
                    print(f"<< Таймаут: получено {pkt_count} пакетов за {retry_count} попыток, нужный не найден")
        return None

    # Если h_adr не задан — ждём первый пакет от ППКУ
    if h_adr is None:
        print("Ожидание пакета от ППКУ (d_type=10) для определения адреса...")
        deadline = time.time() + 15.0
        while time.time() < deadline:
            chunk = ser.read(256)
            for b in chunk:
                result = bsu.feed(b)
                if result:
                    rid = result[0]
                    rdata = result[1]
                    p = parse_can_id(rid)
                    if p["d_type"] == d_type and p["dir"] == 1:
                        h_adr = p["h_adr"]
                        can_id_req = build_can_id(d_type, h_adr, l_adr, zone, 0)
                        can_id_rsp = build_can_id(d_type, h_adr, l_adr, zone, 1)
                        print(f"  ППКУ обнаружен: h_adr={h_adr}")
                        break
            else:
                pass  # временно без sleep
                continue
            break
        else:
            print("Ошибка: ППКУ не обнаружен за 15 с")
            return -1

    # 1. GetConfigSize
    req = bytes([SVC_GET_CONFIG_SIZE]) + b"\x00" * 7
    ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
    print(f">> REQ [{ts}] GetConfigSize  data=[{req.hex()}]")
    rsp = wait_response(req, SVC_GET_CONFIG_SIZE, req_label="GetConfigSize")
    if not rsp or len(rsp) < 5:
        print("  << Ошибка: нет ответа GetConfigSize")
        return -1
    size_bytes = ((rsp[1] << 24) |
                  (rsp[2] << 16) |
                  (rsp[3] << 8)  |
                   rsp[4])
    ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
    print(f"<< RSP [{ts}] GetConfigSize → {size_bytes} байт  data=[{rsp.hex()}]")
    print()

    num_words = (size_bytes + 3) // 4

    # 2. GetConfigWord по каждому слову (перезапрос каждые 5 мс при отсутствии ответа)
    for i in range(num_words):
        req = bytes([SVC_GET_CONFIG_WORD, (i >> 8) & 0xFF, i & 0xFF]) + b"\x00" * 5
        ts_req = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        print(f">> REQ [{ts_req}] GetConfigWord word#{i}  data=[{req.hex()}]")
        rsp = wait_response(req, SVC_GET_CONFIG_WORD, expected_word_idx=i, req_label=f"GetConfigWord word#{i}")
        ts_rsp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        if not rsp or len(rsp) < 7:
            print(f"<< Ошибка: нет ответа GetConfigWord word#{i}")
            return -1
        word = struct.unpack(">I", rsp[3:7])[0]
        pct = (i + 1) * 100 // num_words
        if full_log:
            print(f"<< RSP [{ts_rsp}] GetConfigWord word#{i} 0x{word:08X}  data=[{rsp.hex()}]")
        elif not quiet:
            print(f"<< RSP [{ts_rsp}] word#{i} 0x{word:08X} ({pct}%)")
        elif (i + 1) % 100 == 0 or i == num_words - 1:
            print(f"  ... {pct}% ({i + 1}/{num_words})")

    print()
    print(f"Конфигурация прочитана: {size_bytes} байт ({num_words} слов)")
    return 0


# --- Смещения PPKYCfg / MKUCfg (device_lib/include/device_config.h, ARM GCC) ---
CFG_BASE = 72  # UniqId(32) + beep..isBRP(8) + was_fire(u32) + baudrates + reserv[20] до CfgDevices[0]
PPKY_WAS_FIRE_OFF = 40
PPKY_EX_CAN_BAUD_OFF = 44
PPKY_EX_RS485_BAUD_OFF = 48
PPKY_RESERV_OFF = 52
PPKY_RESERV_BYTES = 20
ZONE_NUMBER_CFG = 100
ZONE_NAME_SIZE_CFG = 64
ZONE_NAME_AREA_CFG = ZONE_NUMBER_CFG * ZONE_NAME_SIZE_CFG  # 6400
FIRE_AND_BYTES_CFG = ZONE_NUMBER_CFG  # uint8_t fire_and[ZONE_NUMBER]
PPKY_TAIL_BYTES_CFG = 2  # beep_block + wifi_block после fire_and[]
NUM_DEV_IN_MCU_CFG = 32
MKU_UID_BYTES = 32
MKU_VDTYPE_BYTES = NUM_DEV_IN_MCU_CFG * 4  # 128
MKU_MODULE_DELAY_BYTES = NUM_DEV_IN_MCU_CFG * 4  # 128
# Начало Devices[0] внутри MKUCfg (после UId + VDtype + zone_delay + module_delay)
MKU_DEVICES0_OFF = MKU_UID_BYTES + MKU_VDTYPE_BYTES + 4 + MKU_MODULE_DELAY_BYTES  # 292
# module_delay[32] идёт сразу после zone_delay (uint32 LE на слот)
MKU_ZONE_DELAY_OFF = MKU_UID_BYTES + MKU_VDTYPE_BYTES  # 160
MKU_MODULE_DELAY_OFF = MKU_ZONE_DELAY_OFF + 4  # 164
MKU_STRIDE_BYTES = MKU_DEVICES0_OFF + NUM_DEV_IN_MCU_CFG * 64 + 64  # 2404 = sizeof(MKUCfg) new
MKU_STRIDE_OLD_BYTES = 1060  # legacy: UId + Devices[16]×(type+reserv[63])
MKU_OLD_DEVICES0_OFF = MKU_UID_BYTES  # Devices[0] сразу после UId
MKU_OLD_MAX_SLOTS = 16
MKU_TOTAL_WORDS = MKU_STRIDE_BYTES // 4  # 601 слов на один MKUCfg (new)
MKU_POST_UID_WORDS = (MKU_STRIDE_BYTES - MKU_UID_BYTES) // 4  # 593 слова после UId

# Слоты виртуальных устройств по типу платы МКУ (device.hpp)
MCU_HARDWARE_SLOTS: dict[int, list[int]] = {
    13: [11, 12],         # МКУ_IGN
    14: [12],             # МКУ_TC
    20: [11, 11, 12],     # МКУ_K1: 2 спички + ДПТ
    21: [11, 11, 11],     # МКУ_K2: 3 спички
    22: [16, 16, 11],     # МКУ_K3: 2 концевика + спичка
    23: [17, 17],         # МКУ_KR: 2 реле
}


def _valid_mku_header(cfg: bytes, off: int) -> bool:
    if off + 24 > len(cfg):
        return False
    if all(cfg[off + k] == 0 for k in range(32)):
        return False
    return cfg[off + 23] in MKU_DEVICE_TYPES


MKU_DEVICE_TYPES = (13, 14, 20, 21, 22, 23)
VD_DEVICE_TYPES = (11, 12, 15, 16, 17)


def _mku_header_count(cfg: bytes, stride: int) -> int:
    count = 0
    for i in range(NUM_DEV_IN_MCU_CFG):
        if not _valid_mku_header(cfg, CFG_BASE + i * stride):
            break
        count += 1
    return count


def _score_mku_device_payload(cfg: bytes, mku_off: int, layout: str) -> int:
    """Оценка правдоподобности данных виртуальных устройств для выбора old/new layout."""
    if mku_off + 24 > len(cfg):
        return 0
    board = cfg[mku_off + 23]
    hw = MCU_HARDWARE_SLOTS.get(board, [])
    slot_count = len(hw) if hw else (MKU_OLD_MAX_SLOTS if layout == "old" else 8)
    score = 0
    for j in range(slot_count):
        if layout == "new":
            vd = _mku_vdtype(cfg, mku_off, j)
            ro = mku_off + MKU_DEVICES0_OFF + j * 64
            if ro + 64 > len(cfg):
                break
            reserv = cfg[ro : ro + 64]
            dtype = vd if vd in VD_DEVICE_TYPES else (hw[j] if j < len(hw) else 0)
        else:
            base = mku_off + MKU_OLD_DEVICES0_OFF + j * 64
            if base + 64 > len(cfg):
                break
            dtype = cfg[base]
            reserv = cfg[base + 1 : base + 64]
        if dtype not in VD_DEVICE_TYPES:
            continue
        if not any(reserv[:32]):
            continue
        if dtype == 11 and len(reserv) >= 6:
            lo = struct.unpack_from("<H", reserv, 2)[0]
            hi = struct.unpack_from("<H", reserv, 4)[0]
            if 100 <= lo <= 10000 and lo <= hi <= 20000:
                score += 4
            else:
                score += 1
        elif dtype in (12, 15, 16, 17):
            score += 2
        else:
            score += 1
    return score


def detect_mku_stride_from_cfg(cfg: bytes, size_raw: int | None = None) -> tuple[int, str]:
    """Выбрать sizeof(MKUCfg): размер с ППКУ, число заголовков МКУ, содержимое слотов."""
    raw = size_raw if size_raw is not None else len(cfg)
    tail = ZONE_NAME_AREA_CFG + FIRE_AND_BYTES_CFG + PPKY_TAIL_BYTES_CFG
    threshold_new = CFG_BASE + NUM_DEV_IN_MCU_CFG * MKU_STRIDE_BYTES + tail - 512

    old_count = _mku_header_count(cfg, MKU_STRIDE_OLD_BYTES)
    new_count = _mku_header_count(cfg, MKU_STRIDE_BYTES)

    candidates: list[tuple[int, str, int]] = [
        (old_count, "old", MKU_STRIDE_OLD_BYTES),
        (new_count, "new", MKU_STRIDE_BYTES),
    ]
    candidates.sort(key=lambda x: x[0], reverse=True)
    best_count, best_layout, best_stride = candidates[0]

    if best_count > 0 and candidates[0][0] == candidates[1][0]:
        score_old = _score_mku_device_payload(cfg, CFG_BASE, "old")
        score_new = _score_mku_device_payload(cfg, CFG_BASE, "new")
        if score_new > score_old:
            best_stride, best_layout = MKU_STRIDE_BYTES, "new"
        elif score_old > score_new:
            best_stride, best_layout = MKU_STRIDE_OLD_BYTES, "old"
        elif raw >= threshold_new:
            best_stride, best_layout = MKU_STRIDE_BYTES, "new"
        else:
            best_stride, best_layout = MKU_STRIDE_OLD_BYTES, "old"

    if best_count > 0:
        if raw < threshold_new - 1000 and best_layout == "new" and old_count >= new_count:
            score_old = _score_mku_device_payload(cfg, CFG_BASE, "old")
            score_new = _score_mku_device_payload(cfg, CFG_BASE, "new")
            if score_old >= score_new:
                return MKU_STRIDE_OLD_BYTES, "old"
        return best_stride, best_layout

    if raw >= threshold_new:
        return MKU_STRIDE_BYTES, "new"
    return MKU_STRIDE_OLD_BYTES, "old"


def _reserv_has_data(reserv: bytes) -> bool:
    return len(reserv) >= 8 and any(reserv[:32])


def _mku_vdtype(cfg: bytes, mku_off: int, slot: int) -> int:
    vd_off = mku_off + MKU_UID_BYTES + slot * 4
    if vd_off + 4 > len(cfg):
        return 0
    return _u32_le_buf(cfg, vd_off) & 0xFF


def iter_mku_virtual_devices(
    cfg: bytes, mku_off: int, mku_stride: int, mku_board_type: int, layout: str
) -> list[tuple[int, int, bytes]]:
    """Список (slot, vd_type, reserv64) для одного MKUCfg."""
    out: list[tuple[int, int, bytes]] = []
    hw = MCU_HARDWARE_SLOTS.get(mku_board_type, [])

    if layout == "new":
        slot_count = len(hw) if hw else NUM_DEV_IN_MCU_CFG
        for j in range(slot_count):
            reserv_off = mku_off + MKU_DEVICES0_OFF + j * 64
            if reserv_off + 64 > len(cfg):
                break
            reserv = cfg[reserv_off : reserv_off + 64]
            vd_type = _mku_vdtype(cfg, mku_off, j)
            if not vd_type and j < len(hw):
                vd_type = hw[j]
            if not vd_type and not _reserv_has_data(reserv):
                continue
            if not vd_type:
                continue
            out.append((j, vd_type, reserv))
        return out

    max_slots = min(MKU_OLD_MAX_SLOTS, max(0, (mku_stride - MKU_OLD_DEVICES0_OFF) // 64))
    for j in range(max_slots):
        base = mku_off + MKU_OLD_DEVICES0_OFF + j * 64
        if base + 64 > len(cfg):
            break
        vd_type = cfg[base]
        reserv = bytearray(64)
        reserv[:63] = cfg[base + 1 : base + 64]
        if not vd_type and not _reserv_has_data(reserv):
            continue
        if not vd_type:
            continue
        out.append((j, vd_type, bytes(reserv)))
    return out

# Минимальный размер PPKYCfg по device_config.h (чтобы хвост fire_and/beep_block помещался в буфер)
MIN_PPKY_CFG_BYTES = (
    CFG_BASE + NUM_DEV_IN_MCU_CFG * MKU_STRIDE_BYTES + ZONE_NAME_AREA_CFG
    + FIRE_AND_BYTES_CFG + PPKY_TAIL_BYTES_CFG
)

SVC_SET_CONFIG_WORD = 153
SVC_SAVE_CONFIG = 154
MKU_UID_WORD_COUNT = MKU_UID_BYTES // 4  # 8 слов UniqId


def _dpt_reserv_cfg(mode: int = 0, use_max: int = 1, max_c: int = 60, delay_ms: int = 100) -> bytes:
    reserv = bytearray(64)
    reserv[0] = mode & 0xFF
    reserv[1] = use_max & 0xFF
    struct.pack_into("<H", reserv, 2, max_c)
    struct.pack_into("<H", reserv, 4, delay_ms)
    return bytes(reserv)


def _igniter_reserv_cfg(
    low: int = 100, high: int = 1000, retry: int = 0, disable_sc: int = 0
) -> bytes:
    reserv = bytearray(64)
    reserv[0] = disable_sc & 0xFF
    struct.pack_into("<H", reserv, 2, low)
    struct.pack_into("<H", reserv, 4, high)
    reserv[6] = retry & 0xFF
    return bytes(reserv)


def _relay_reserv_cfg(settle_ms: int = 100) -> bytes:
    reserv = bytearray(64)
    struct.pack_into("<H", reserv, 4, settle_ms)
    return bytes(reserv)


def _button_k3_reserv_cfg() -> bytes:
    """MCU_k3_v097 DefaultConfig: Devices[0]."""
    reserv = bytearray(64)
    reserv[0] = 2  # mode: кнопка
    reserv[1] = 0  # use_max
    struct.pack_into("<H", reserv, 2, 60)
    struct.pack_into("<H", reserv, 4, 100)
    reserv[6] = 0  # DeviceButtonKind_StartSP
    return bytes(reserv)


def _lswitch_k3_reserv_cfg() -> bytes:
    """MCU_k3_v097 DefaultConfig: Devices[1] (голова DPT + нули в хвосте)."""
    reserv = bytearray(64)
    reserv[0] = 1  # mode: концевик
    reserv[1] = 0  # use_max
    struct.pack_into("<H", reserv, 2, 60)
    struct.pack_into("<H", reserv, 4, 100)
    return bytes(reserv)


def _pack_mkucfg_body(
    vdtypes: list[int],
    zone_delay: int,
    module_delays: list[int],
    device_slots: dict[int, bytes],
) -> bytes:
    """MKUCfg без UId: VDtype, задержки, Devices[], reserv[64] (как после memset+DefaultConfig)."""
    body = bytearray(MKU_STRIDE_BYTES - MKU_UID_BYTES)
    for slot, vd in enumerate(vdtypes[:NUM_DEV_IN_MCU_CFG]):
        struct.pack_into("<I", body, slot * 4, vd & 0xFF)
    zd_rel = MKU_ZONE_DELAY_OFF - MKU_UID_BYTES
    struct.pack_into("<I", body, zd_rel, zone_delay)
    md_rel = MKU_MODULE_DELAY_OFF - MKU_UID_BYTES
    for slot, md in enumerate(module_delays[:NUM_DEV_IN_MCU_CFG]):
        struct.pack_into("<I", body, md_rel + slot * 4, md)
    dev_rel = MKU_DEVICES0_OFF - MKU_UID_BYTES
    for slot, reserv in device_slots.items():
        if 0 <= slot < NUM_DEV_IN_MCU_CFG:
            body[dev_rel + slot * 64 : dev_rel + slot * 64 + 64] = reserv[:64]
    return bytes(body)


def build_mku_factory_cfg(d_type: int) -> bytes | None:
    """
    Полный MKUCfg (2404 байта) как DefaultConfig() в прошивке соответствующей платы.
    UId обнулён — подставляется из устройства перед записью.
    Источники: MCU_k1/k2/k3/kr_v097, MCU_TC Core/Src/app.cpp.
    """
    ign = _igniter_reserv_cfg()
    if d_type == 20:  # MCU_K1
        body = _pack_mkucfg_body(
            vdtypes=[12, 11, 11],
            zone_delay=5,
            module_delays=[0, 2, 3],
            device_slots={
                0: _dpt_reserv_cfg(mode=0, use_max=1, max_c=60, delay_ms=100),
                1: ign,
                2: ign,
            },
        )
    elif d_type == 21:  # MCU_K2
        body = _pack_mkucfg_body(
            vdtypes=[11, 11, 11],
            zone_delay=5,
            module_delays=[0, 2, 4],
            device_slots={0: ign, 1: ign, 2: ign},
        )
    elif d_type == 22:  # MCU_K3
        body = _pack_mkucfg_body(
            vdtypes=[15, 16, 11],
            zone_delay=5,
            module_delays=[0, 0, 2],
            device_slots={0: _button_k3_reserv_cfg(), 1: _lswitch_k3_reserv_cfg(), 2: ign},
        )
    elif d_type == 23:  # MCU_KR
        relay = _relay_reserv_cfg()
        body = _pack_mkucfg_body(
            vdtypes=[17, 17],
            zone_delay=0,
            module_delays=[],
            device_slots={0: relay, 1: relay},
        )
    elif d_type == 14:  # MCU_TC
        body = _pack_mkucfg_body(
            vdtypes=[12],
            zone_delay=0,
            module_delays=[],
            device_slots={
                0: _dpt_reserv_cfg(mode=0, use_max=1, max_c=100, delay_ms=100),
            },
        )
    else:
        return None
    cfg = bytearray(MKU_STRIDE_BYTES)
    cfg[MKU_UID_BYTES:] = body
    return bytes(cfg)


def _config_io_timeouts(transport_hint: str) -> tuple[float, float]:
    th = (transport_hint or "auto").strip().lower()
    if th == "wifi":
        return 5.0, 0.02
    return 2.0, 0.006


def _wait_device_config_response(
    ser,
    bsu: BSUParser,
    can_id_req: int,
    req_data: bytes,
    expected_cmd: int,
    expected_word_idx: int | None = None,
    transport_hint: str = "auto",
) -> bytes | None:
    """Ожидание сервисного ответа Get/SetConfigWord от конкретного устройства."""
    total_timeout, retry_ms = _config_io_timeouts(transport_hint)
    target = parse_can_id(can_id_req)
    old_timeout = ser.timeout
    ser.timeout = 0
    deadline = time.time() + total_timeout
    try:
        while time.time() < deadline:
            pkt = build_bsu_can_packet(can_id_req, req_data)
            ser.write(pkt)
            retry_deadline = time.time() + retry_ms
            while time.time() < retry_deadline:
                chunk = ser.read(512)
                for b in chunk:
                    result = bsu.feed(b)
                    if not result:
                        continue
                    rid, rdata = result[0], result[1]
                    if len(rdata) == 0 or rdata[0] != expected_cmd:
                        continue
                    p = parse_can_id(rid)
                    if p["dir"] != 1:
                        continue
                    if (
                        p["d_type"] != target["d_type"]
                        or p["h_adr"] != target["h_adr"]
                        or p["l_adr"] != target["l_adr"]
                        or p["zone"] != target["zone"]
                    ):
                        continue
                    if expected_word_idx is not None and len(rdata) >= 3:
                        got_idx = (rdata[1] << 8) | rdata[2]
                        if got_idx != expected_word_idx:
                            continue
                    return rdata
                if not chunk:
                    time.sleep(0.001)
    finally:
        ser.timeout = old_timeout
    return None


def read_mku_config_word(
    ser,
    bsu: BSUParser,
    d_type: int,
    h_adr: int,
    l_adr: int,
    zone: int,
    word_idx: int,
    transport_hint: str = "auto",
) -> int | None:
    can_id = build_can_id(d_type, h_adr, l_adr, zone, 0)
    req = bytes([SVC_GET_CONFIG_WORD, (word_idx >> 8) & 0xFF, word_idx & 0xFF]) + b"\x00" * 5
    rsp = _wait_device_config_response(
        ser, bsu, can_id, req, SVC_GET_CONFIG_WORD, expected_word_idx=word_idx, transport_hint=transport_hint
    )
    if not rsp or len(rsp) < 7:
        return None
    return struct.unpack(">I", rsp[3:7])[0]


def write_mku_config_word(
    ser,
    bsu: BSUParser,
    d_type: int,
    h_adr: int,
    l_adr: int,
    zone: int,
    word_idx: int,
    word_be: int,
    transport_hint: str = "auto",
) -> bool:
    can_id = build_can_id(d_type, h_adr, l_adr, zone, 0)
    req = bytes([
        SVC_SET_CONFIG_WORD,
        (word_idx >> 8) & 0xFF,
        word_idx & 0xFF,
        (word_be >> 24) & 0xFF,
        (word_be >> 16) & 0xFF,
        (word_be >> 8) & 0xFF,
        word_be & 0xFF,
    ]) + b"\x00" * 2
    rsp = _wait_device_config_response(
        ser, bsu, can_id, req, SVC_SET_CONFIG_WORD, expected_word_idx=word_idx, transport_hint=transport_hint
    )
    return rsp is not None


def save_mku_config(
    ser,
    bsu: BSUParser,
    d_type: int,
    h_adr: int,
    l_adr: int,
    zone: int,
    transport_hint: str = "auto",
) -> bool:
    can_id = build_can_id(d_type, h_adr, l_adr, zone, 0)
    req = bytes([SVC_SAVE_CONFIG]) + b"\x00" * 7
    rsp = _wait_device_config_response(
        ser, bsu, can_id, req, SVC_SAVE_CONFIG, transport_hint=transport_hint
    )
    return rsp is not None


def apply_mku_factory_defaults(
    ser,
    bsu: BSUParser,
    d_type: int,
    h_adr: int,
    l_adr: int,
    zone: int,
    transport_hint: str = "auto",
    progress_callback=None,
) -> bool:
    """Сбросить MKUCfg как DefaultConfig() в прошивке платы, сохранив только UId (адрес/ID)."""
    uid = bytearray(MKU_UID_BYTES)
    for word_idx in range(MKU_UID_WORD_COUNT):
        word_be = read_mku_config_word(
            ser, bsu, d_type, h_adr, l_adr, zone, word_idx, transport_hint=transport_hint
        )
        if word_be is None:
            return False
        struct.pack_into(">I", uid, word_idx * 4, word_be)

    factory = build_mku_factory_cfg(d_type)
    if factory is None:
        return False

    new_cfg = bytearray(factory)
    new_cfg[:MKU_UID_BYTES] = uid

    for n, word_idx in enumerate(range(MKU_TOTAL_WORDS)):
        pos = word_idx * 4
        word_be = struct.unpack(">I", new_cfg[pos : pos + 4])[0]
        if not write_mku_config_word(
            ser, bsu, d_type, h_adr, l_adr, zone, word_idx, word_be, transport_hint=transport_hint
        ):
            return False
        if progress_callback:
            progress_callback(n + 1, MKU_TOTAL_WORDS)

    return save_mku_config(ser, bsu, d_type, h_adr, l_adr, zone, transport_hint=transport_hint)


def apply_mku_factory_defaults_all(
    ser,
    bsu: BSUParser,
    mku_list: list[tuple[int, int, int, int]],
    transport_hint: str = "auto",
    mku_progress_callback=None,
) -> tuple[int, int]:
    """Применить заводские настройки к списку МКУ: [(d_type, h, l, zone), ...]."""
    ok = 0
    total = len(mku_list)
    for i, mku in enumerate(mku_list):
        d_type, h_adr, l_adr, zone = mku
        if mku_progress_callback:
            mku_progress_callback(i, total, mku)
        if apply_mku_factory_defaults(
            ser,
            bsu,
            d_type,
            h_adr,
            l_adr,
            zone,
            transport_hint=transport_hint,
        ):
            ok += 1
    return ok, total


def read_config_bytes(
    ser, bsu: BSUParser, h_adr: int, l_adr: int = 0, zone: int = 0,
    progress_callback=None,
    word_burst_size: int = 128,
    word_burst_collect_sec: float = 0.30,
    word_burst_rounds: int = 3,
    transport_hint: str = "auto",
) -> tuple[bytes | None, int]:
    """
    Читает конфигурацию с ППКУ, возвращает (config_bytes, size) или (None, 0) при ошибке.
    Оптимизация:
      - чтение CfgDevices[i] обрывается по первому полностью нулевому UniqId МКУ;
      - внутри занятого MKUCfg читается полный блок sizeof(MKUCfg) (VDtype, задержки, Devices, reserv);
      - имена зон — до первой полностью нулевой зоны;
      - затем байты fire_and[ZONE_NUMBER].
    """
    d_type = DEVICE_PPKY_TYPE
    current_h_adr = h_adr
    can_id_req = build_can_id(d_type, current_h_adr, l_adr, zone, 0)

    def send_req(data: bytes, broadcast: bool = False) -> None:
        # Для сервисного чтения конфига надёжнее broadcast-запрос:
        # ответ всё равно приходит от конкретного ППКУ (dir=1), а адрес
        # можно извлечь из ответа и зафиксировать в current_h_adr.
        req_id = build_can_id(d_type, 0, 0, 0, 0) if broadcast else can_id_req
        pkt = build_bsu_can_packet(req_id, data)
        ser.write(pkt)

    # Профили транспорта:
    # - WiFi/TCP: больше задержки и "окна" ожидания;
    # - USB: более быстрый цикл запрос/ответ.
    th = (transport_hint or "auto").strip().lower()
    is_wifi_profile = (th == "wifi")
    if is_wifi_profile:
        RETRY_TIMEOUT_MS = 0.02
        TOTAL_TIMEOUT_SEC = 5.0
    else:
        RETRY_TIMEOUT_MS = 0.006
        TOTAL_TIMEOUT_SEC = 2.0
    WORD_BURST_SIZE = max(1, int(word_burst_size))
    WORD_BURST_COLLECT_SEC = max(0.01, float(word_burst_collect_sec))
    WORD_BURST_ROUNDS = max(1, int(word_burst_rounds))

    def wait_response(
        req_data: bytes,
        expected_cmd: int,
        expected_word_idx: int | None = None,
        broadcast_req: bool = False,
        accept_any_ppky_addr: bool = True,
    ) -> bytes | None:
        nonlocal current_h_adr, can_id_req
        old_timeout = ser.timeout
        ser.timeout = 0  # non-blocking
        deadline = time.time() + TOTAL_TIMEOUT_SEC
        try:
            while time.time() < deadline:
                send_req(req_data, broadcast=broadcast_req)
                retry_deadline = time.time() + RETRY_TIMEOUT_MS
                while time.time() < retry_deadline:
                    chunk = ser.read(512)
                    for b in chunk:
                        result = bsu.feed(b)
                        if result:
                            rid = result[0]
                            rdata = result[1]
                            if len(rdata) == 0 or rdata[0] != expected_cmd:
                                continue

                            p = parse_can_id(rid)
                            if p["d_type"] != d_type or p["dir"] != 1:
                                continue

                            if accept_any_ppky_addr:
                                current_h_adr = p["h_adr"]
                                can_id_req = build_can_id(d_type, current_h_adr, l_adr, zone, 0)

                            if expected_word_idx is not None and len(rdata) >= 3:
                                got_idx = (rdata[1] << 8) | rdata[2]
                                if got_idx != expected_word_idx:
                                    continue
                            return rdata
                    if not chunk:
                        time.sleep(0.001)
        finally:
            ser.timeout = old_timeout
        return None

    # --- 0. Узнаём полный размер конфига ---
    req = bytes([SVC_GET_CONFIG_SIZE]) + b"\x00" * 7
    if is_wifi_profile:
        # Для WiFi обычно безопаснее стартовать с broadcast.
        rsp = wait_response(req, SVC_GET_CONFIG_SIZE, broadcast_req=True, accept_any_ppky_addr=True)
    else:
        # Для USB сначала пробуем адресный запрос (если h_adr уже задан),
        # затем fallback на broadcast для автопоиска ППКУ.
        rsp = wait_response(
            req,
            SVC_GET_CONFIG_SIZE,
            broadcast_req=(current_h_adr == 0),
            accept_any_ppky_addr=(current_h_adr == 0),
        )
        if (not rsp or len(rsp) < 5):
            rsp = wait_response(req, SVC_GET_CONFIG_SIZE, broadcast_req=True, accept_any_ppky_addr=True)
    if not rsp or len(rsp) < 5:
        return (None, 0)
    size_bytes_raw = ((rsp[1] << 24) |
                      (rsp[2] << 16) |
                      (rsp[3] << 8)  |
                       rsp[4])
    # Буфер расширяем при необходимости, но stride MKUCfg определяем по фактическому размеру с ППКУ.
    size_bytes = size_bytes_raw if size_bytes_raw >= MIN_PPKY_CFG_BYTES else max(size_bytes_raw, MIN_PPKY_CFG_BYTES)

    # --- 1. Базовые параметры структуры (PPKYCfg: CfgDevices[32]×sizeof(MKUCfg)) ---
    ZONE_NAME_SIZE = ZONE_NAME_SIZE_CFG
    ZONE_NUMBER = ZONE_NUMBER_CFG
    ZONE_NAME_AREA = ZONE_NAME_AREA_CFG
    # Stride MKUCfg по заявленному размеру с ППКУ (до расширения буфера).
    ppky_tail = ZONE_NAME_AREA_CFG + FIRE_AND_BYTES_CFG + PPKY_TAIL_BYTES_CFG
    if size_bytes_raw >= CFG_BASE + NUM_DEV_IN_MCU_CFG * MKU_STRIDE_BYTES + ppky_tail - 512:
        MKUCFG_STRIDE = MKU_STRIDE_BYTES
    else:
        MKUCFG_STRIDE = MKU_STRIDE_OLD_BYTES
    MKU_BLOCK_WORDS = (MKUCFG_STRIDE + 3) // 4
    zone_name_offset = CFG_BASE + NUM_DEV_IN_MCU_CFG * MKUCFG_STRIDE

    num_words = (size_bytes + 3) // 4
    if progress_callback:
        progress_callback(0, 0, num_words)

    # Кэш прочитанных слов: word_idx -> uint32
    cache: dict[int, int] = {}

    def _collect_word_responses(wanted: set[int], collect_sec: float) -> None:
        """Собрать ответы GetConfigWord из потока в cache по индексам wanted."""
        if not wanted:
            return
        deadline = time.time() + collect_sec
        while time.time() < deadline and wanted:
            chunk = ser.read(1024)
            if not chunk:
                time.sleep(0.001)
                continue
            for b in chunk:
                result = bsu.feed(b)
                if not result:
                    continue
                rid = result[0]
                rdata = result[1]
                if len(rdata) < 7 or rdata[0] != SVC_GET_CONFIG_WORD:
                    continue
                p = parse_can_id(rid)
                if p["d_type"] != d_type or p["dir"] != 1:
                    continue
                got_idx = (rdata[1] << 8) | rdata[2]
                if got_idx not in wanted:
                    continue
                cache[got_idx] = struct.unpack(">I", rdata[3:7])[0]
                wanted.discard(got_idx)

    def fetch_words_burst(indices: list[int]) -> bool:
        """Запросить пачку слов, затем дозапросить только пропущенные."""
        pending = [idx for idx in indices if idx not in cache and 0 <= idx < num_words]
        if not pending:
            return True

        # 1) Основные раунды burst-запросов
        rounds = 0
        while pending and rounds < WORD_BURST_ROUNDS:
            rounds += 1
            batch = pending[:WORD_BURST_SIZE]
            wanted = set(batch)
            for idx in batch:
                req = bytes([SVC_GET_CONFIG_WORD, (idx >> 8) & 0xFF, idx & 0xFF]) + b"\x00" * 5
                send_req(req, broadcast=False)
            _collect_word_responses(wanted, WORD_BURST_COLLECT_SEC)
            pending = [idx for idx in pending if idx not in cache]

        # 2) Точечный дозапрос пропущенных индексов
        for idx in pending:
            req = bytes([SVC_GET_CONFIG_WORD, (idx >> 8) & 0xFF, idx & 0xFF]) + b"\x00" * 5
            rsp = wait_response(req, SVC_GET_CONFIG_WORD, expected_word_idx=idx, broadcast_req=False)
            if not rsp or len(rsp) < 7:
                return False
            cache[idx] = struct.unpack(">I", rsp[3:7])[0]
        return True

    def fetch_word(idx: int) -> int | None:
        """Прочитать одно слово конфига по индексу (0..num_words-1) с кэшем."""
        if idx in cache:
            return cache[idx]
        if idx < 0 or idx >= num_words:
            return None
        # Для любого промаха кэша запрашиваем окно слов пачкой.
        # Это ускоряет "умные" участки обхода (UID, имена зон, fire_and),
        # где чтение идёт через fetch_word(), а не через store_word().
        burst_end = min(idx + WORD_BURST_SIZE, num_words)
        if not fetch_words_burst(list(range(idx, burst_end))):
            return None
        return cache.get(idx)

    config = bytearray(size_bytes)

    # Для прогресса считаем максимум — все слова
    total_words = num_words if num_words > 0 else 1
    words_read = 0

    def store_word(idx: int) -> bool:
        """Прочитать слово idx и записать его в config. False при ошибке."""
        nonlocal words_read
        w = fetch_word(idx)
        if w is None:
            return False
        pos = idx * 4
        if pos + 4 <= size_bytes:
            struct.pack_into(">I", config, pos, w)
        words_read += 1
        if progress_callback:
            pct = (words_read * 100) // total_words
            progress_callback(pct, words_read, total_words)
        return True

    # --- 2. Заголовок ППКУ ---
    header_words = (CFG_BASE + 3) // 4
    for i in range(header_words):
        if not store_word(i):
            return (None, 0)

    # --- 3. MKUCfg по-блочно, обрываем по нулевому UniqId; внутри блока — полный sizeof(MKUCfg) ---
    UID_WORDS = MKU_UID_BYTES // 4  # 8 слов
    for i in range(NUM_DEV_IN_MCU_CFG):
        base_off = CFG_BASE + i * MKUCFG_STRIDE
        base_idx = base_off // 4
        if base_off >= zone_name_offset or base_idx >= num_words:
            break

        # 3.1 UID[8 слов]
        uid_zero = True
        for w_i in range(UID_WORDS):
            idx = base_idx + w_i
            w = fetch_word(idx)
            if w is None:
                return (None, 0)
            pos = idx * 4
            if pos + 4 <= size_bytes:
                struct.pack_into(">I", config, pos, w)
            words_read += 1
            uid_zero = uid_zero and (w == 0)
        if progress_callback:
            pct = (words_read * 100) // total_words
            progress_callback(pct, words_read, total_words)

        if uid_zero:
            break

        # 3.2 Остаток MKUCfg: VDtype/Devices, задержки, reserv
        for idx in range(base_idx + UID_WORDS, base_idx + MKU_BLOCK_WORDS):
            if idx >= num_words:
                break
            if not store_word(idx):
                return (None, 0)

    # --- 4. Имена зон, обрываем по первой полностью нулевой зоне ---
    for z in range(ZONE_NUMBER):
        zone_off = zone_name_offset + z * ZONE_NAME_SIZE
        if zone_off >= size_bytes:
            break
        zone_idx0 = zone_off // 4
        if zone_idx0 >= num_words:
            break

        all_zero = True
        for w_i in range(ZONE_NAME_SIZE // 4):  # 16 слов = 64 байта
            idx = zone_idx0 + w_i
            if idx >= num_words:
                break
            w = fetch_word(idx)
            if w is None:
                return (None, 0)
            pos = idx * 4
            if pos + 4 <= size_bytes:
                struct.pack_into(">I", config, pos, w)
            words_read += 1
            all_zero = all_zero and (w == 0)
        if progress_callback:
            pct = (words_read * 100) // total_words
            progress_callback(pct, words_read, total_words)

        if all_zero:
            break

    # --- 5. fire_and[ZONE_NUMBER], beep_block, wifi_block (хвост PPKYCfg) ---
    fire_off = zone_name_offset + ZONE_NAME_AREA
    tail_bytes = FIRE_AND_BYTES_CFG + PPKY_TAIL_BYTES_CFG
    tail_words = (tail_bytes + 3) // 4
    if fire_off + tail_bytes <= size_bytes:
        fire_idx0 = fire_off // 4
        for w_i in range(tail_words):
            idx = fire_idx0 + w_i
            if idx >= num_words:
                break
            if not store_word(idx):
                return (None, 0)

    # Возвращаем фактическую длину буфера (после возможного расширения до MIN_PPKY_CFG_BYTES)
    out_len = len(config)
    return (bytes(config[:out_len]), size_bytes_raw)


# device_config.h: типы устройств (совпадает с device.hpp, device_config.h)
DEVICE_NAMES_CFG = {
    0: "—",
    10: "ППКУ",
    11: "Спичка",
    12: "ДПТ",
    13: "МКУ_IGN",
    14: "МКУ_TC",
    17: "Реле",
    20: "МКУ_K1",
    21: "МКУ_K2",
    22: "МКУ_K3",
    23: "МКУ_KR",
    15: "Кнопка",
    16: "Концевик",
}


def _device_name(t: int) -> str:
    return DEVICE_NAMES_CFG.get(t, f"type{t}")


def dump_config_hex(cfg: bytes, max_bytes: int = 256) -> list[str]:
    """Отладочный дамп: hex байт по 16 в строке."""
    lines: list[str] = []
    for i in range(0, min(len(cfg), max_bytes), 16):
        chunk = cfg[i : i + 16]
        hex_str = " ".join(f"{b:02x}" for b in chunk)
        ascii_str = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"  {i:04x}: {hex_str:<48} {ascii_str}")
    return lines


def _u16_le_buf(b: bytes, off: int) -> int:
    return struct.unpack_from("<H", b, off)[0]


def _u32_le_buf(b: bytes, off: int) -> int:
    return struct.unpack_from("<I", b, off)[0]


def _button_kind_name(kind: int) -> str:
    names = ("ПУСК СП", "пуск всех зон", "пуск по списку зон")
    return names[kind] if 0 <= kind < len(names) else str(kind)


def _lswitch_function_name(func: int) -> str:
    names = {
        1: "неисправность",
        2: "ручной ППКУ",
        3: "авто ППКУ",
        4: "пауза пуска",
    }
    return names.get(func, str(func))


def _device_cfg_extras(vd_type: int, reserv: bytes) -> str:
    """Краткое описание Device*Config внутри VDeviceCfg::reserv (64 байта, LE)."""
    if len(reserv) < 8:
        return ""
    parts: list[str] = []
    if vd_type == 11:  # DeviceIgniterConfig: uint8 + pad + 2×uint16 + uint8 + …
        disable = reserv[0]
        th_lo = _u16_le_buf(reserv, 2)
        th_hi = _u16_le_buf(reserv, 4)
        retry = reserv[6]
        parts.append(f"пороги={th_lo}-{th_hi}мВ retry={retry} КЗ_чек={'выкл' if disable else 'вкл'}")
    elif vd_type == 12:  # DeviceDPTConfig
        mode = reserv[0]
        use_max = reserv[1]
        th = _u16_le_buf(reserv, 2)
        dms = _u16_le_buf(reserv, 4)
        parts.append(f"режим={mode} MAX={'да' if use_max else 'нет'} T_пож={th}°C стаб={dms}мс")
    elif vd_type == 17:  # DeviceRelayConfig
        settle = _u16_le_buf(reserv, 4)
        mode = reserv[6] if len(reserv) > 6 else 0
        mode_names = ("нет авто", "по пожару", "по неисправности", "по концевику")
        mode_s = mode_names[mode] if mode < len(mode_names) else str(mode)
        saved = reserv[7] if len(reserv) > 7 else 0
        parts.append(
            f"init={reserv[0]} persist={reserv[1]} inv_ОС={reserv[2]} "
            f"задержка_перекл={reserv[3]}с ожид_ОС={settle}мс mode={mode}({mode_s}) saved={saved}"
        )
    elif vd_type == 15:  # DeviceButtonConfig (голова DPT + button_kind/zones/NC)
        dms = _u16_le_buf(reserv, 4)
        kind = reserv[6] if len(reserv) > 6 else 0
        zones = list(reserv[7:14]) if len(reserv) >= 14 else []
        nc = reserv[14] if len(reserv) > 14 else 0
        zones_nz = [z for z in zones if z != 0]
        zones_s = ",".join(str(z) for z in zones_nz) if zones_nz else "—"
        parts.append(
            f"kind={kind}({_button_kind_name(kind)}) зоны=[{zones_s}] "
            f"{'NC' if nc else 'NO'} стаб={dms}мс"
        )
    elif vd_type == 16:  # DeviceLimitSwitchConfig
        use_max = reserv[1]
        th = _u16_le_buf(reserv, 2)
        dms = _u16_le_buf(reserv, 4)
        trig = reserv[6]
        func = reserv[7]
        nc = reserv[8]
        parts.append(
            f"MAX={'да' if use_max else 'нет'} T={th}°C стаб={dms}мс "
            f"trig={trig}с func={func}({_lswitch_function_name(func)}) "
            f"{'NC' if nc else 'NO'}"
        )
    elif vd_type != 0 and any(reserv[:16]):
        hx = reserv[:8].hex()
        parts.append(f"reserv[:8]={hx}")
    return " | ".join(parts)


def parse_config_display(
    cfg: bytes, debug_dump: bool = False, config_size_raw: int | None = None
) -> list[str]:
    """
    Парсит PPKYCfg (device_config.h) и возвращает список строк с полями.
    PPKYCfg: UniqId(32), beep..isBRP, was_fire(u32), baudrates, reserv[20], CfgDevices[32]×MKUCfg,
    zone_name[100][64], fire_and[100], beep_block, wifi_block.
    MKUCfg: UId, VDtype[32], zone_delay, module_delay[32], Devices[32]×64, reserv[64].
    """
    lines: list[str] = []
    mku_stride, mku_layout = detect_mku_stride_from_cfg(cfg, config_size_raw)
    zone_name_offset = CFG_BASE + NUM_DEV_IN_MCU_CFG * mku_stride
    fire_and_offset = zone_name_offset + ZONE_NAME_AREA_CFG
    min_full = fire_and_offset + FIRE_AND_BYTES_CFG + PPKY_TAIL_BYTES_CFG

    if debug_dump:
        lines.append("--- Дамп байт 0..255 (отладка) ---")
        lines.extend(dump_config_hex(cfg, 256))
        tail = len(cfg) - fire_and_offset - FIRE_AND_BYTES_CFG - PPKY_TAIL_BYTES_CFG
        mku_guess = tail // NUM_DEV_IN_MCU_CFG if tail > 0 else 0
        lines.append(
            f"--- size={len(cfg)} min_full≈{min_full} CFG_BASE={CFG_BASE} "
            f"MKU_stride={mku_stride}({mku_layout}) zone_off={zone_name_offset} "
            f"fire_and_off={fire_and_offset} ---"
        )
        if len(cfg) >= CFG_BASE + 24:
            off0 = CFG_BASE
            lines.append(
                f"  MKU[0] devId: z={cfg[off0 + 20]} l={cfg[off0 + 21]} h={cfg[off0 + 22]} "
                f"d_type={cfg[off0 + 23]} VDtype[0]={_u32_le_buf(cfg, off0 + 32)}"
            )
        lines.append("---")

    if len(cfg) < 40:
        return lines

    # ППКУ UId (первые 32 байта): devId в offset 20-23
    if len(cfg) >= 24:
        ppky_zone = cfg[20]
        ppky_l = cfg[21]
        ppky_h = cfg[22]
        ppky_dtype = cfg[23]
        lines.append(f"ППКУ: {_device_name(ppky_dtype)} h={ppky_h} l={ppky_l} z={ppky_zone}")

    if len(cfg) < CFG_BASE:
        return lines

    beep = cfg[32]
    fire_mode = cfg[33]
    power_input = cfg[34]
    power_value = cfg[35]
    rs485_on = cfg[36]
    ex_can_on = cfg[37]
    ex_can_protocol = cfg[38]
    is_brp = cfg[39]
    was_fire = _u32_le_buf(cfg, PPKY_WAS_FIRE_OFF) if len(cfg) >= PPKY_WAS_FIRE_OFF + 4 else 0
    ex_can_baud = _u32_le_buf(cfg, PPKY_EX_CAN_BAUD_OFF) if len(cfg) >= PPKY_EX_CAN_BAUD_OFF + 4 else 0
    ex_rs485_baud = _u32_le_buf(cfg, PPKY_EX_RS485_BAUD_OFF) if len(cfg) >= PPKY_EX_RS485_BAUD_OFF + 4 else 0
    fm = ("авто", "автоном", "ручной")
    fm_s = fm[fire_mode] if fire_mode < len(fm) else str(fire_mode)
    can_proto = ("J1939", "J1979")
    can_proto_s = can_proto[ex_can_protocol] if ex_can_protocol < len(can_proto) else str(ex_can_protocol)
    lines.append(
        f"beep={beep} fire_mode={fire_mode}({fm_s}) power: вводов={power_input} U={power_value}В "
        f"rs485={rs485_on} ex_can={ex_can_on} протокол_can={ex_can_protocol}({can_proto_s}) "
        f"isBRP={is_brp} was_fire={was_fire}"
    )
    if ex_can_on or ex_can_baud:
        lines.append(f"ex_can_baudrate={ex_can_baud}")
    if rs485_on or ex_rs485_baud:
        lines.append(f"ex_rs485_baudrate={ex_rs485_baud}")

    for i in range(NUM_DEV_IN_MCU_CFG):
        off = CFG_BASE + i * mku_stride
        if off + MKU_UID_BYTES > len(cfg):
            break
        zone = cfg[off + 20]
        l_adr = cfg[off + 21]
        h_adr = cfg[off + 22]
        d_type = cfg[off + 23]
        uid_empty = all(cfg[off + k] == 0 for k in range(32))
        if uid_empty:
            break

        d_name = _device_name(d_type)
        header = f"CfgDevices[{i}]: {d_name} h={h_adr} l={l_adr} z={zone}"
        if mku_layout == "new" and off + MKU_ZONE_DELAY_OFF + 4 <= len(cfg):
            zd = _u32_le_buf(cfg, off + MKU_ZONE_DELAY_OFF)
            mod_delays: list[str] = []
            for j in range(NUM_DEV_IN_MCU_CFG):
                vd_type_j = _mku_vdtype(cfg, off, j)
                if vd_type_j == 0:
                    continue
                if off + MKU_MODULE_DELAY_OFF + j * 4 + 4 <= len(cfg):
                    md_j = _u32_le_buf(cfg, off + MKU_MODULE_DELAY_OFF + j * 4)
                    mod_delays.append(f"сл.{j}:{md_j}с")
            md_part = f" zone_delay={zd}с" if zd else ""
            if mod_delays:
                md_part += f" module_delay={','.join(mod_delays)}"
            header += md_part
        lines.append(header)

        for slot, vd_type, reserv in iter_mku_virtual_devices(cfg, off, mku_stride, d_type, mku_layout):
            extras = _device_cfg_extras(vd_type, reserv)
            suf = f" — {extras}" if extras else ""
            lines.append(f"  dev[{slot}] {_device_name(vd_type)}{suf}")

    # Имена зон
    for z in range(ZONE_NUMBER_CFG):
        off = zone_name_offset + z * ZONE_NAME_SIZE_CFG
        if off + ZONE_NAME_SIZE_CFG > len(cfg):
            break
        name_bytes = cfg[off : off + ZONE_NAME_SIZE_CFG]
        name = name_bytes.split(b"\x00")[0].decode("utf-8", errors="replace").strip()
        if not name:
            break
        lines.append(f"zone_name[{z}]: {name!r}")

    # fire_and[ZONE_NUMBER] + beep_block + wifi_block
    fire_off = fire_and_offset
    if fire_off + FIRE_AND_BYTES_CFG <= len(cfg):
        fire_and = cfg[fire_off : fire_off + FIRE_AND_BYTES_CFG]
        and_zones = [str(zi) for zi, v in enumerate(fire_and) if v != 0]
        if and_zones:
            preview = ", ".join(and_zones[:40])
            more = f" …(+{len(and_zones) - 40})" if len(and_zones) > 40 else ""
            lines.append(f"fire_and (режим «И», ненулевые зоны): {preview}{more}")
        else:
            lines.append("fire_and: ни одна зона не в режиме «И» (везде «ИЛИ»)")
        if debug_dump:
            lines.append(f"fire_and[0]={fire_and[0]} (offset={fire_off}, word#{fire_off // 4})")
            if len(cfg) >= fire_off + 4:
                w208 = struct.unpack_from(">I", cfg, fire_off)[0]
                lines.append(
                    f"fire_and сырьё: слово#{(fire_off // 4)} BE=0x{w208:08X}, "
                    f"байты[0..3]={cfg[fire_off : fire_off + 4].hex()}"
                )

    tail_off = fire_off + FIRE_AND_BYTES_CFG
    if tail_off + PPKY_TAIL_BYTES_CFG <= len(cfg):
        beep_block = cfg[tail_off]
        wifi_block = cfg[tail_off + 1]
        lines.append(f"beep_block={beep_block} wifi_block={wifi_block}")

    return lines


def main():
    parser = argparse.ArgumentParser(description="Монитор шины BSU (CAN over USB)")
    parser.add_argument("port", nargs="?", help="COM-порт (например COM3)")
    parser.add_argument("-b", "--baud", type=int, default=1000000, help="Скорость (по умолчанию 1 Мбит/с)")
    parser.add_argument("--list", action="store_true", help="Показать доступные COM-порты")
    parser.add_argument("--raw", action="store_true", help="Дополнительно выводить сырые байты")
    parser.add_argument("--id", action="store_true", help="Показывать сырой CAN ID (hex) для отладки")
    parser.add_argument("--be-id", action="store_true", help="CAN ID в big-endian (если парсинг неверный)")
    parser.add_argument("--show-svc", action="store_true", help="Показывать сервисные 128/129 (по умолчанию скрыты)")
    parser.add_argument("--read-config", action="store_true", help="Читать конфигурацию с ППКУ (протокол backend)")
    parser.add_argument("--config-quiet", action="store_true", help="Меньше вывода при чтении конфигурации (прогресс каждые 500 слов)")
    parser.add_argument("--config-debug", action="store_true", help="Отладка: выводить все пакеты при ожидании ответа GetConfigWord")
    parser.add_argument("--config-log", action="store_true", help="Полный лог: каждый запрос и каждый пакет — отдельная строка")
    parser.add_argument("--h-adr", type=int, default=None, metavar="N", help="Адрес ППКУ (h_adr). Без указания — ждать пакет от ППКУ")
    args = parser.parse_args()

    if args.list:
        ports = serial.tools.list_ports.comports()
        if not ports:
            print("COM-порты не найдены")
        else:
            for p in ports:
                print(f"  {p.device} — {p.description}")
        return

    if not args.port:
        parser.error("Укажите COM-порт или --list")
        return

    try:
        # В режиме чтения конфигурации — таймаут 1 мс (устройство отвечает быстро)
        ser_timeout = 0 if args.read_config else 0.1  # 0 = non-blocking, минимум задержек
        ser = serial.Serial(args.port, args.baud, timeout=ser_timeout)
    except serial.SerialException as e:
        print(f"Ошибка открытия {args.port}: {e}")
        sys.exit(1)

    print(f"Монитор шины BSU: {args.port} @ {args.baud}")
    if args.read_config:
        print("Режим: чтение конфигурации с ППКУ")
    print("Ctrl+C для выхода")
    print("-" * 60)

    bsu = BSUParser(be_id=args.be_id)
    try:
        if args.read_config:
            run_read_config(ser, bsu, args.h_adr, quiet=args.config_quiet, debug=args.config_debug, full_log=args.config_log)
            return
        while True:
            chunk = ser.read(256)
            if not chunk:
                continue
            for b in chunk:
                result = bsu.feed(b)
                if result:
                    can_id, data, bus_label = result
                    if not args.show_svc and len(data) > 0 and data[0] in (128, 129):
                        continue  # Скрыть ResetMCU/StopStartSend
                    ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
                    line = format_packet(can_id, data, show_raw_id=args.id, bus_label=bus_label)
                    print(f"[{ts}] {line}")
                    if args.raw:
                        print(f"       RAW: ID=0x{can_id:08X} {data.hex()}")
    except KeyboardInterrupt:
        print("\nВыход")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
