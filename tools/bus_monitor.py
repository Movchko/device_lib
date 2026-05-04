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
        # CAN data[2]   = power (канал 4)   — шаг 100 мВ (code * 0.1 В, 198 -> 19.8 В)
        # CAN data[3]   = Rpower (канал 0) — шаг 100 мВ
        # CAN data[4]   = current1         — шаг 50 мА (code * 0.05 А)
        # CAN data[5]   = current2         — шаг 50 мА
        if len(data) >= 6:
            sec = data[1]
            power_code = data[2]
            rpower_code = data[3]
            cur1_code = data[4]
            cur2_code = data[5]
            u = power_code / 10.0      # В
            ur = rpower_code / 10.0    # В
            i1 = cur1_code * 0.05      # А
            i2 = cur2_code * 0.05      # А
            return _dev_line(
                f"  {dev_str} | t={sec}s U={u:.1f}V U_res={ur:.1f}V I1={i1:.2f}A I2={i2:.2f}A"
            )
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
        # CAN data[2..3]= measured_resistance_ohm (LE, 16 бит), Ом
        # CAN data[4]   = max_temp_tc_c (°C, int8)
        # CAN data[5]   = max_fault_mask (битовая маска: FAULT/SCV/SCG/OC)
        # CAN data[6]   = max_temp_internal_c (°C, int8)
        if len(data) >= 6:
            line_code = data[1]
            line = DPT_LINE.get(line_code, "?")
            resistance = data[2] | (data[3] << 8)
            max_temp_tc = data[4] - 256 if data[4] >= 128 else data[4]
            max_fault_mask = data[5]
            max_temp_int = (data[6] - 256 if len(data) >= 7 and data[6] >= 128 else (data[6] if len(data) >= 7 else 0))
            active_flags = [name for bit, name in MAX_FAULT_FLAGS if (max_fault_mask & bit)]
            flags_str = "|".join(active_flags) if active_flags else "OK"
            return _dev_line(
                f"  {dev_str} | line={line} R={resistance}Ω MAX(tc={max_temp_tc}°C,int={max_temp_int}°C,mask=0x{max_fault_mask:02X}:{flags_str})"
            )
        return _dev_line(f"  {dev_str} | DPT status (len={len(data)})")
    if parsed["d_type"] == 15 and parsed["dir"]:  # Кнопка (на базе ДПТ) →
        # Формат как у DPT: data[1]=LineState, data[2..3]=R, data[4]=tc, data[5]=fault_mask, data[6]=int
        if len(data) >= 6:
            line_code = data[1]
            line = BUTTON_LINE.get(line_code, f"code{line_code}")
            resistance = data[2] | (data[3] << 8)
            max_fault_mask = data[5]
            active_flags = [name for bit, name in MAX_FAULT_FLAGS if (max_fault_mask & bit)]
            flags_str = "|".join(active_flags) if active_flags else "OK"
            return _dev_line(f"  {dev_str} | line={line} R={resistance}Ω MAXmask=0x{max_fault_mask:02X}:{flags_str}")
        return _dev_line(f"  {dev_str} | Button status (len={len(data)})")
    if parsed["d_type"] == 16 and parsed["dir"]:  # Концевик (на базе ДПТ) →
        if len(data) >= 6:
            line_code = data[1]
            line = LSWITCH_LINE.get(line_code, f"code{line_code}")
            resistance = data[2] | (data[3] << 8)
            max_fault_mask = data[5]
            active_flags = [name for bit, name in MAX_FAULT_FLAGS if (max_fault_mask & bit)]
            flags_str = "|".join(active_flags) if active_flags else "OK"
            return _dev_line(f"  {dev_str} | line={line} R={resistance}Ω MAXmask=0x{max_fault_mask:02X}:{flags_str}")
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
        # МКУ → (tick 4b LE, CAN flags в data[5], для K1/K2 также U24 в data[6])
        if len(data) >= 6:
            # status_data[0..3]=tick LE, [4]=CAN_flags → SendMessage: data[0]=cmd, data[1..4]=tick, data[5]=flags
            tick = struct.unpack_from("<I", data, 1)[0]
            can_flags = int(data[5])  # явно int на случай list/array
            can1 = "✓" if (can_flags & 0x01) else "—"
            can2 = "✓" if (can_flags & 0x02) else "—"
            if parsed["d_type"] in (20, 21, 22, 23) and len(data) >= 7:
                u24_code_01v = int(data[6])  # 0.1V шаг как у ППКУ
                u24_v = u24_code_01v / 10.0
                return _dev_line(
                    f"  {dev_str} | tick={tick} CAN1={can1} CAN2={can2} U24={u24_v:.1f}V"
                )
            return _dev_line(f"  {dev_str} | tick={tick} CAN1={can1} CAN2={can2}")
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
            if self.total < 12 or self.type_val not in (BSU_PKT_TYPE_CAN, BSU_PKT_TYPE_CAN2):
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
        ser.reset_input_buffer()
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
CFG_BASE = 72  # sizeof(UniqId) + beep..isBRP + reserv[32] до CfgDevices[0]
ZONE_NUMBER_CFG = 100
ZONE_NAME_SIZE_CFG = 64
ZONE_NAME_AREA_CFG = ZONE_NUMBER_CFG * ZONE_NAME_SIZE_CFG  # 6400
FIRE_AND_BYTES_CFG = ZONE_NUMBER_CFG  # uint8_t fire_and[ZONE_NUMBER]
NUM_DEV_IN_MCU_CFG = 32
MKU_UID_BYTES = 32
MKU_VDTYPE_BYTES = NUM_DEV_IN_MCU_CFG * 4  # 128
MKU_MODULE_DELAY_BYTES = NUM_DEV_IN_MCU_CFG * 4  # 128
# Начало Devices[0] внутри MKUCfg (после UId + VDtype + zone_delay + module_delay)
MKU_DEVICES0_OFF = MKU_UID_BYTES + MKU_VDTYPE_BYTES + 4 + MKU_MODULE_DELAY_BYTES  # 292
# module_delay[32] идёт сразу после zone_delay (uint32 LE на слот)
MKU_ZONE_DELAY_OFF = MKU_UID_BYTES + MKU_VDTYPE_BYTES  # 160
MKU_MODULE_DELAY_OFF = MKU_ZONE_DELAY_OFF + 4  # 164
MKU_STRIDE_BYTES = MKU_DEVICES0_OFF + NUM_DEV_IN_MCU_CFG * 64 + 64  # 2404 = sizeof(MKUCfg)
MKU_TOTAL_WORDS = MKU_STRIDE_BYTES // 4  # 601 слов на один MKUCfg
MKU_POST_UID_WORDS = (MKU_STRIDE_BYTES - MKU_UID_BYTES) // 4  # 593 слова после UId

# Минимальный размер PPKYCfg по device_config.h (чтобы fire_and всегда помещался в буфер и читался по словам)
MIN_PPKY_CFG_BYTES = CFG_BASE + NUM_DEV_IN_MCU_CFG * MKU_STRIDE_BYTES + ZONE_NAME_AREA_CFG + FIRE_AND_BYTES_CFG


def read_config_bytes(
    ser, bsu: BSUParser, h_adr: int, l_adr: int = 0, zone: int = 0,
    progress_callback=None,
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
    can_id_req = build_can_id(d_type, h_adr, l_adr, zone, 0)
    can_id_rsp = build_can_id(d_type, h_adr, l_adr, zone, 1)

    def send_req(data: bytes) -> None:
        pkt = build_bsu_can_packet(can_id_req, data)
        ser.write(pkt)

    RETRY_TIMEOUT_MS = 0.005
    TOTAL_TIMEOUT_SEC = 2.0

    def wait_response(req_data: bytes, expected_cmd: int, expected_word_idx: int | None = None) -> bytes | None:
        old_timeout = ser.timeout
        ser.timeout = 0  # non-blocking
        deadline = time.time() + TOTAL_TIMEOUT_SEC
        try:
            while time.time() < deadline:
                send_req(req_data)
                retry_deadline = time.time() + RETRY_TIMEOUT_MS
                while time.time() < retry_deadline:
                    chunk = ser.read(512)
                    for b in chunk:
                        result = bsu.feed(b)
                        if result:
                            rid = result[0]
                            rdata = result[1]
                            if rid == can_id_rsp and len(rdata) > 0 and rdata[0] == expected_cmd:
                                if expected_word_idx is not None and len(rdata) >= 3:
                                    got_idx = (rdata[1] << 8) | rdata[2]
                                    if got_idx != expected_word_idx:
                                        continue
                                return rdata
                    if not chunk:
                        pass
        finally:
            ser.timeout = old_timeout
        return None

    # --- 0. Узнаём полный размер конфига ---
    req = bytes([SVC_GET_CONFIG_SIZE]) + b"\x00" * 7
    rsp = wait_response(req, SVC_GET_CONFIG_SIZE)
    if not rsp or len(rsp) < 5:
        return (None, 0)
    size_bytes = ((rsp[1] << 24) |
                  (rsp[2] << 16) |
                  (rsp[3] << 8)  |
                   rsp[4])
    # Если прошивка отдаёт размер без хвоста (или с усечённым округлением), расширяем буфер
    # до минимума, иначе слова fire_and (слово #20850 при стандартном sizeof) не попадают в буфер.
    if size_bytes < MIN_PPKY_CFG_BYTES:
        size_bytes = MIN_PPKY_CFG_BYTES

    # --- 1. Базовые параметры структуры (PPKYCfg: CfgDevices[32] фикс. sizeof(MKUCfg)) ---
    ZONE_NAME_SIZE = ZONE_NAME_SIZE_CFG
    ZONE_NUMBER = ZONE_NUMBER_CFG
    ZONE_NAME_AREA = ZONE_NAME_AREA_CFG
    MKUCFG_STRIDE = MKU_STRIDE_BYTES
    zone_name_offset = CFG_BASE + NUM_DEV_IN_MCU_CFG * MKUCFG_STRIDE

    num_words = (size_bytes + 3) // 4

    # Кэш прочитанных слов: word_idx -> uint32
    cache: dict[int, int] = {}

    def fetch_word(idx: int) -> int | None:
        """Прочитать одно слово конфига по индексу (0..num_words-1) с кэшем."""
        if idx in cache:
            return cache[idx]
        if idx < 0 or idx >= num_words:
            return None
        ser.reset_input_buffer()
        req = bytes([SVC_GET_CONFIG_WORD, (idx >> 8) & 0xFF, idx & 0xFF]) + b"\x00" * 5
        rsp = wait_response(req, SVC_GET_CONFIG_WORD, expected_word_idx=idx)
        if not rsp or len(rsp) < 7:
            return None
        word = struct.unpack(">I", rsp[3:7])[0]
        cache[idx] = word
        return word

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

        # 3.2 Остаток MKUCfg: VDtype, zone_delay, module_delay, Devices[32], reserv[64]
        for idx in range(base_idx + UID_WORDS, base_idx + MKU_TOTAL_WORDS):
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

    # --- 5. fire_and[ZONE_NUMBER] (после имён зон) ---
    fire_off = zone_name_offset + ZONE_NAME_AREA
    fire_words = (FIRE_AND_BYTES_CFG + 3) // 4
    if fire_off + FIRE_AND_BYTES_CFG <= size_bytes:
        fire_idx0 = fire_off // 4
        for w_i in range(fire_words):
            idx = fire_idx0 + w_i
            if idx >= num_words:
                break
            if not store_word(idx):
                return (None, 0)

    # Возвращаем фактическую длину буфера (после возможного расширения до MIN_PPKY_CFG_BYTES)
    out_len = len(config)
    return (bytes(config[:out_len]), out_len)


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
        parts.append(
            f"init={reserv[0]} persist={reserv[1]} inv_ОС={reserv[2]} задержка_перекл={reserv[3]}с ожид_ОС={settle}мс"
        )
    elif vd_type == 15:  # DeviceButtonConfig
        kind = reserv[0]
        zones = list(reserv[1:8])
        parts.append(f"button_kind={kind} zones={zones}")
    elif vd_type == 16:  # DeviceLimitSwitchConfig
        mode = reserv[0]
        use_max = reserv[1]
        th = _u16_le_buf(reserv, 2)
        dms = _u16_le_buf(reserv, 4)
        trig = reserv[6]
        func = reserv[7]
        nc = reserv[8]
        parts.append(
            f"режим={mode} MAX={'да' if use_max else 'нет'} T={th}°C стаб={dms}мс "
            f"trig={trig}s func={func} NC={nc}"
        )
    elif vd_type != 0 and any(reserv[:16]):
        hx = reserv[:8].hex()
        parts.append(f"reserv[:8]={hx}")
    return " | ".join(parts)


def parse_config_display(cfg: bytes, debug_dump: bool = False) -> list[str]:
    """
    Парсит PPKYCfg (device_config.h) и возвращает список строк с полями.
    PPKYCfg: UniqId(32), beep, fire_mode, power_*, rs485/ex_can, isBRP, reserv[32],
    CfgDevices[32]×MKUCfg, zone_name[100][64], fire_and[100].
    MKUCfg: UId, VDtype[32], zone_delay, module_delay[32], Devices[32]×64, reserv[64].
    """
    lines: list[str] = []
    zone_name_offset = CFG_BASE + NUM_DEV_IN_MCU_CFG * MKU_STRIDE_BYTES
    min_full = zone_name_offset + ZONE_NAME_AREA_CFG + FIRE_AND_BYTES_CFG

    if debug_dump:
        lines.append("--- Дамп байт 0..255 (отладка) ---")
        lines.extend(dump_config_hex(cfg, 256))
        tail = len(cfg) - CFG_BASE - ZONE_NAME_AREA_CFG - FIRE_AND_BYTES_CFG
        mku_guess = tail // NUM_DEV_IN_MCU_CFG if tail > 0 else 0
        lines.append(
            f"--- size={len(cfg)} min_full≈{min_full} MKU_stride={MKU_STRIDE_BYTES} "
            f"tail_for_mkus={tail} → tail/32={mku_guess} zone_off={zone_name_offset} ---"
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
    fm = ("авто", "автоном", "ручной")
    fm_s = fm[fire_mode] if fire_mode < len(fm) else str(fire_mode)
    lines.append(
        f"beep={beep} fire_mode={fire_mode}({fm_s}) power: вводов={power_input} U={power_value}В "
        f"rs485={rs485_on} ex_can={ex_can_on} протокол_can={ex_can_protocol} isBRP={is_brp}"
    )

    for i in range(NUM_DEV_IN_MCU_CFG):
        off = CFG_BASE + i * MKU_STRIDE_BYTES
        if off + MKU_STRIDE_BYTES > len(cfg):
            break
        zone = cfg[off + 20]
        l_adr = cfg[off + 21]
        h_adr = cfg[off + 22]
        d_type = cfg[off + 23]
        uid_empty = all(cfg[off + k] == 0 for k in range(32))
        if uid_empty:
            break

        zd = _u32_le_buf(cfg, off + MKU_ZONE_DELAY_OFF)
        d_name = _device_name(d_type)
        mod_delays: list[str] = []
        for j in range(NUM_DEV_IN_MCU_CFG):
            vd_type_j = _u32_le_buf(cfg, off + 32 + j * 4)
            if vd_type_j == 0:
                continue
            md_j = _u32_le_buf(cfg, off + MKU_MODULE_DELAY_OFF + j * 4)
            mod_delays.append(f"{md_j}с")
        md_part = f" module_delay={','.join(mod_delays)}" if mod_delays else ""
        lines.append(
            f"CfgDevices[{i}]: {d_name} h={h_adr} l={l_adr} z={zone} zone_delay={zd}с{md_part}"
        )

        for j in range(NUM_DEV_IN_MCU_CFG):
            vd_off = off + 32 + j * 4
            vd_type = _u32_le_buf(cfg, vd_off)
            if vd_type == 0:
                continue
            dev_res_off = off + MKU_DEVICES0_OFF + j * 64
            reserv = cfg[dev_res_off : dev_res_off + 64] if dev_res_off + 64 <= len(cfg) else b""
            extras = _device_cfg_extras(vd_type, reserv)
            suf = f" — {extras}" if extras else ""
            lines.append(f"  dev[{j}] {_device_name(vd_type)}{suf}")

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

    # fire_and[ZONE_NUMBER] (байт 83400 = fire_and[0] при sizeof(MKUCfg)=2404)
    fire_off = zone_name_offset + ZONE_NAME_AREA_CFG
    if fire_off + FIRE_AND_BYTES_CFG <= len(cfg):
        fire_and = cfg[fire_off : fire_off + FIRE_AND_BYTES_CFG]
        lines.append(f"fire_and[0]={fire_and[0]} (offset={fire_off}, word#{fire_off // 4})")
        if len(cfg) >= fire_off + 4:
            w208 = struct.unpack_from(">I", cfg, fire_off)[0]
            lines.append(
                f"fire_and сырьё: слово#{(fire_off // 4)} BE=0x{w208:08X}, "
                f"байты[0..3]={cfg[fire_off : fire_off + 4].hex()}"
            )
        and_zones = [str(zi) for zi, v in enumerate(fire_and) if v != 0]
        if and_zones:
            preview = ", ".join(and_zones[:40])
            more = f" …(+{len(and_zones) - 40})" if len(and_zones) > 40 else ""
            lines.append(f"fire_and (режим «И», ненулевые зоны): {preview}{more}")
        else:
            lines.append("fire_and: ни одна зона не в режиме «И» (везде «ИЛИ»)")

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
