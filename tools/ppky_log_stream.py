"""
ppky_log_stream.py — протокол потоковой выгрузки лога ППКУ (UART2 / WiFi).

Совместим с log_stream.c / log_transport.c прошивки stm_PPKY.
"""

from __future__ import annotations

import struct
import time
from queue import Empty, Queue
from typing import Callable, Iterator

from bus_monitor import bsu_checksum, format_device, parse_can_id

BSU_PREAMBLE = (0x55, 0xAA)
BSU_HEADER_SIZE = 8
BSU_CHECKSUM_SIZE = 2
BSU_LOG_MAX_PKT_SIZE = 256
BSU_LOG_BODY_MAX = 246

LOG_PKT_TYPE_REQ = 16
LOG_PKT_TYPE_RSP = 17
LOG_PKT_TYPE_DATA = 18

LOG_OPCODE_PING = 0x01
LOG_OPCODE_GET_INFO = 0x02
LOG_OPCODE_START_DUMP = 0x10
LOG_OPCODE_STOP_DUMP = 0x11
LOG_OPCODE_GET_DUMP_STATUS = 0x12

LOG_STATUS_OK = 0
LOG_STATUS_NOT_INIT = 1
LOG_STATUS_BAD_PARAM = 2
LOG_STATUS_OUT_OF_RANGE = 3
LOG_STATUS_BUSY = 4
LOG_STATUS_FORBIDDEN = 5
LOG_STATUS_INTERNAL = 6

LOG_TIER_CRITICAL = 0
LOG_TIER_GENERAL = 1
LOG_TIER_BOTH = 2

LOG_STREAM_FLAG_FIRST = 0x01
LOG_STREAM_FLAG_LAST = 0x02
LOG_STREAM_FLAG_TIER_CHANGE = 0x04
LOG_STREAM_FLAG_ERROR = 0x80

LOG_STREAM_HDR_SIZE = 16
LOG_STREAM_REC_SIZE = 38
EVENT_LOG_RECORD_SIZE = 32

LOG_STATUS_NAMES = {
    LOG_STATUS_OK: "OK",
    LOG_STATUS_NOT_INIT: "NOT_INIT",
    LOG_STATUS_BAD_PARAM: "BAD_PARAM",
    LOG_STATUS_OUT_OF_RANGE: "OUT_OF_RANGE",
    LOG_STATUS_BUSY: "BUSY",
    LOG_STATUS_FORBIDDEN: "FORBIDDEN",
    LOG_STATUS_INTERNAL: "INTERNAL",
}

LOG_TIER_NAMES = {
    LOG_TIER_CRITICAL: "КРИТ",
    LOG_TIER_GENERAL: "ОБЩ",
}

REC_STATUS_NAMES = {
    0: "OK",
    1: "EMPTY",
    2: "BAD",
}

EVENT_LOG_REC_VALID = 0
EVENT_LOG_REC_EMPTY = 1
EVENT_LOG_REC_INVALID = 2

EVENT_LOG_NAMES = {
    1: "MASTER_START",
    2: "MASTER_STOP",
    3: "SYSTEM_START_OK",
    4: "DEVICE_MISSING",
    5: "DEVICE_FOUND",
    6: "CONFIG_MISMATCH",
    7: "TELEMETRY",
    8: "DEVICE_FAULT",
    9: "FIRE_DETECTED",
    10: "EXTINGUISH_START",
    11: "EXTINGUISH_FORCE_STOP",
    12: "EXTINGUISH_COMPLETE",
    13: "EXTINGUISH_INCOMPLETE",
    14: "PANEL_BUTTON",
}


def build_bsu_log_frame(pkt_type: int, seq: int, payload: bytes = b"") -> bytes:
    """Собрать BSU-кадр (preamble + size + type + seq + body + CRC)."""
    if len(payload) > BSU_LOG_BODY_MAX:
        raise ValueError(f"payload too large: {len(payload)}")
    pkt_size = BSU_HEADER_SIZE + len(payload) + BSU_CHECKSUM_SIZE
    if pkt_size > BSU_LOG_MAX_PKT_SIZE:
        raise ValueError(f"frame too large: {pkt_size}")
    out = bytearray()
    out.append(BSU_PREAMBLE[0])
    out.append(BSU_PREAMBLE[1])
    out.extend(struct.pack("<HHH", pkt_size, pkt_type, seq))
    out.extend(payload)
    crc = bsu_checksum(bytes(out))
    out.extend(struct.pack("<H", crc))
    return bytes(out)


def build_log_request(seq: int, opcode: int, extra: bytes = b"") -> bytes:
    return build_bsu_log_frame(LOG_PKT_TYPE_REQ, seq, bytes([opcode]) + extra)


def can_frame_from_bsu_body(body: bytes) -> tuple[int, bytes, str] | None:
    """Из тела BSU CAN-кадра (12 байт) извлечь can_id и data."""
    if len(body) < 12:
        return None
    can_id = struct.unpack("<I", body[:4])[0] & 0x1FFFFFFF
    data = bytes(body[4:12])
    return can_id, data, "CAN1"


class BsuLogFrameParser:
    """Парсер входящих BSU-кадров (type 0/1/17/18, размер до 256 байт)."""

    ACCEPT_TYPES = (0, 1, 17, 18)

    def __init__(self):
        self.state = "PREAMBLE_0"
        self.pkt_size = 0
        self.pkt_type = 0
        self.pkt_seq = 0
        self.body_total = 0
        self.body_pos = 0
        self.crc_acc = 0
        self.body_buf = bytearray()
        self.crc_lo = 0

    def reset(self) -> None:
        self.state = "PREAMBLE_0"
        self.body_pos = 0

    def feed(self, b: int) -> dict | None:
        if self.state == "PREAMBLE_0":
            if b == BSU_PREAMBLE[0]:
                self.state = "PREAMBLE_1"
            return None

        if self.state == "PREAMBLE_1":
            if b == BSU_PREAMBLE[1]:
                self.state = "SIZE_LO"
                self.crc_acc = BSU_PREAMBLE[0] + BSU_PREAMBLE[1]
            else:
                self.reset()
            return None

        if self.state == "SIZE_LO":
            self.pkt_size = b
            self.crc_acc = (self.crc_acc + b) & 0xFFFF
            self.state = "SIZE_HI"
            return None

        if self.state == "SIZE_HI":
            self.pkt_size |= b << 8
            self.crc_acc = (self.crc_acc + b) & 0xFFFF
            self.state = "TYPE_LO"
            return None

        if self.state == "TYPE_LO":
            self.pkt_type = b
            self.crc_acc = (self.crc_acc + b) & 0xFFFF
            self.state = "TYPE_HI"
            return None

        if self.state == "TYPE_HI":
            self.pkt_type |= b << 8
            self.crc_acc = (self.crc_acc + b) & 0xFFFF
            self.state = "SEQ_LO"
            return None

        if self.state == "SEQ_LO":
            self.pkt_seq = b
            self.crc_acc = (self.crc_acc + b) & 0xFFFF
            self.state = "SEQ_HI"
            return None

        if self.state == "SEQ_HI":
            self.pkt_seq |= b << 8
            self.crc_acc = (self.crc_acc + b) & 0xFFFF
            min_size = BSU_HEADER_SIZE + BSU_CHECKSUM_SIZE
            if self.pkt_size < min_size or self.pkt_size > BSU_LOG_MAX_PKT_SIZE:
                self.reset()
                return None
            self.body_total = self.pkt_size - min_size
            if self.body_total > BSU_LOG_BODY_MAX:
                self.reset()
                return None
            self.body_pos = 0
            self.body_buf = bytearray(self.body_total)
            self.state = "BODY"
            return None

        if self.state == "BODY":
            self.body_buf[self.body_pos] = b
            self.body_pos += 1
            self.crc_acc = (self.crc_acc + b) & 0xFFFF
            if self.body_pos >= self.body_total:
                self.state = "CRC_LO"
            return None

        if self.state == "CRC_LO":
            self.crc_lo = b
            self.state = "CRC_HI"
            return None

        if self.state == "CRC_HI":
            recv_crc = self.crc_lo | (b << 8)
            calc_crc = self.crc_acc & 0xFFFF
            pkt_type = self.pkt_type
            pkt_seq = self.pkt_seq
            body = bytes(self.body_buf)
            self.reset()
            if recv_crc != calc_crc:
                return None
            if pkt_type not in self.ACCEPT_TYPES:
                return None
            return {"type": pkt_type, "seq": pkt_seq, "body": body}

        self.reset()
        return None


def is_record_missing(status: int, rec_bytes: bytes) -> bool:
    """Запись отсутствует: пустой сектор Flash (0xFF) или битая CRC."""
    if status != EVENT_LOG_REC_VALID:
        return True
    if len(rec_bytes) < EVENT_LOG_RECORD_SIZE:
        return True
    if all(b == 0xFF for b in rec_bytes):
        return True
    event_code = struct.unpack_from("<H", rec_bytes, 8)[0]
    if event_code == 0xFFFF:
        return True
    return False


def _bcd_byte(value: int) -> int:
    return ((value >> 4) & 0x0F) * 10 + (value & 0x0F)


def format_bcd_time(time_bcd: bytes) -> str:
    if len(time_bcd) < 6 or all(x == 0 for x in time_bcd[:6]):
        return "??/??/?? ??:??:??"
    yy = _bcd_byte(time_bcd[0])
    mo = _bcd_byte(time_bcd[1])
    dd = _bcd_byte(time_bcd[2])
    hh = _bcd_byte(time_bcd[3])
    mm = _bcd_byte(time_bcd[4])
    ss = _bcd_byte(time_bcd[5])
    return f"20{yy:02d}-{mo:02d}-{dd:02d} {hh:02d}:{mm:02d}:{ss:02d}"


PANEL_BUTTON_NAMES = {
    0: "ПУСК_ОБЩИЙ",
    1: "ПУСК_СП",
    2: "ОСТАНОВ_ПУСКА",
}


def format_event_record(logical_idx: int, status: int, tier: int, rec: bytes) -> str:
    if len(rec) < EVENT_LOG_RECORD_SIZE:
        return f"#{logical_idx:05d}  <short record {len(rec)}B>"

    time_bcd = rec[0:6]
    wagon = rec[6]
    event_code = struct.unpack_from("<H", rec, 8)[0]
    can_header = struct.unpack_from("<I", rec, 10)[0]
    can_data = rec[14:22]
    additional = rec[22:30]

    ts = format_bcd_time(time_bcd)
    tier_s = LOG_TIER_NAMES.get(tier, f"T{tier}")
    st_s = REC_STATUS_NAMES.get(status, f"S{status}")
    ev_s = EVENT_LOG_NAMES.get(event_code, f"CODE_{event_code}")

    detail = ""
    if event_code == 14:
        btn = PANEL_BUTTON_NAMES.get(additional[0], f"BTN_{additional[0]}")
        zone = additional[1]
        hold = additional[3]
        detail = f"  {btn}"
        if zone:
            detail += f" zone={zone}"
        if hold:
            detail += f" hold={hold}s"

    can_part = ""
    if can_header != 0:
        parsed = parse_can_id(can_header)
        dev = format_device(parsed)
        cmd = can_data[0] if can_data else 0
        can_part = f"  {dev} cmd={cmd} data=[{can_data.hex()}]"

    wagon_part = f"  вагон={wagon}" if wagon else ""
    return f"#{logical_idx:05d}  {ts}  {tier_s} {st_s}  {ev_s}{detail}{wagon_part}{can_part}"


def parse_data_packet(body: bytes) -> tuple[dict, list[tuple[int, int, int, bytes]]]:
    """Разобрать DATA-кадр: заголовок потока и список записей."""
    if len(body) < LOG_STREAM_HDR_SIZE:
        raise ValueError("DATA body too short")
    stream_id, pkt_num, flags, tier, rec_count, _reserved = struct.unpack_from("<HHBBBB", body, 0)
    first_logical = struct.unpack_from("<I", body, 8)[0]
    write_head = struct.unpack_from("<I", body, 12)[0]
    header = {
        "stream_id": stream_id,
        "pkt_num": pkt_num,
        "flags": flags,
        "tier": tier,
        "rec_count": rec_count,
        "first_logical": first_logical,
        "write_head": write_head,
    }
    records: list[tuple[int, int, int, bytes]] = []
    pos = LOG_STREAM_HDR_SIZE
    for _ in range(rec_count):
        if pos + LOG_STREAM_REC_SIZE > len(body):
            break
        logical_idx = struct.unpack_from("<I", body, pos)[0]
        status = body[pos + 4]
        rec_tier = body[pos + 5]
        rec_bytes = body[pos + 6 : pos + 6 + EVENT_LOG_RECORD_SIZE]
        records.append((logical_idx, status, rec_tier, rec_bytes))
        pos += LOG_STREAM_REC_SIZE
    return header, records


def format_get_info_rsp(body: bytes) -> str:
    if len(body) < 2:
        return "GET_INFO: пустой ответ"
    status = body[0]
    if status != LOG_STATUS_OK:
        return f"GET_INFO: {LOG_STATUS_NAMES.get(status, status)}"
    if len(body) < 32:
        return f"GET_INFO: короткий ответ ({len(body)}B)"
    crit_cap, crit_cnt, crit_wh = struct.unpack_from("<III", body, 4)
    gen_cap, gen_cnt, gen_wh = struct.unpack_from("<III", body, 16)
    cat_crc = struct.unpack_from("<I", body, 28)[0]
    return (
        f"КРИТ: {crit_cnt}/{crit_cap} (head={crit_wh})  |  "
        f"ОБЩ: {gen_cnt}/{gen_cap} (head={gen_wh})  |  catalog_crc=0x{cat_crc:08X}"
    )


def drain_queue(q: Queue) -> None:
    while True:
        try:
            q.get_nowait()
        except Empty:
            break


def wait_log_packet(
    q: Queue,
    timeout: float,
    predicate: Callable[[dict], bool],
) -> dict:
    deadline = time.time() + timeout
    while time.time() < deadline:
        remaining = deadline - time.time()
        if remaining <= 0:
            break
        try:
            pkt = q.get(timeout=min(0.1, remaining))
        except Empty:
            continue
        if predicate(pkt):
            return pkt
    raise TimeoutError("таймаут ожидания кадра лога")


class PpkyLogClient:
    """Клиент лог-протокола поверх serial/TCP."""

    def __init__(
        self,
        write_fn: Callable[[bytes, str], bool],
        packet_queue: Queue,
    ):
        self._write = write_fn
        self._queue = packet_queue
        self._seq = 1

    def _next_seq(self) -> int:
        seq = self._seq
        self._seq = (self._seq + 1) & 0xFFFF
        if self._seq == 0:
            self._seq = 1
        return seq

    def send_request(self, opcode: int, extra: bytes = b"", label: str = "") -> int:
        seq = self._next_seq()
        frame = build_log_request(seq, opcode, extra)
        if not self._write(frame, label or f"LOG opcode=0x{opcode:02X}"):
            raise OSError("не удалось отправить запрос лога")
        return seq

    def wait_rsp(self, seq: int, timeout: float = 5.0) -> bytes:
        pkt = wait_log_packet(
            self._queue,
            timeout,
            lambda p: p["type"] == LOG_PKT_TYPE_RSP and p["seq"] == seq,
        )
        return pkt["body"]

    def ping(self, timeout: float = 5.0) -> str:
        drain_queue(self._queue)
        seq = self.send_request(LOG_OPCODE_PING, label="LOG PING")
        try:
            body = self.wait_rsp(seq, timeout)
        except TimeoutError:
            pending = self._queue.qsize()
            return (
                f"[!] PING: нет ответа за {timeout:.0f}с "
                f"(в очереди RX: {pending} кадров). "
                "Проверьте WiFi на ППКУ и прошивку с поддержкой логов."
            )
        if len(body) < 4 or body[0] != LOG_STATUS_OK:
            st = body[0] if body else "?"
            return f"PING: ошибка ({LOG_STATUS_NAMES.get(st, st)})"
        return f"PING OK, proto={body[2]}, record_size={body[3]}B"

    def get_info(self, timeout: float = 8.0) -> str:
        drain_queue(self._queue)
        seq = self.send_request(LOG_OPCODE_GET_INFO, label="LOG GET_INFO")
        try:
            body = self.wait_rsp(seq, timeout)
        except TimeoutError:
            pending = self._queue.qsize()
            return f"[!] GET_INFO: нет ответа за {timeout:.0f}с (в очереди RX: {pending} кадров)"
        return format_get_info_rsp(body)

    def stop_dump(self, timeout: float = 3.0) -> None:
        try:
            seq = self.send_request(LOG_OPCODE_STOP_DUMP, label="LOG STOP_DUMP")
            self.wait_rsp(seq, timeout)
        except (TimeoutError, OSError):
            pass

    def _begin_dump(
        self, tier: int, start_logical: int, rsp_timeout: float
    ) -> tuple[int, int, int, int, str] | str:
        """START_DUMP; вернуть (stream_id, total) или строку ошибки."""
        extra = struct.pack("<BI", tier, start_logical)
        seq = self.send_request(LOG_OPCODE_START_DUMP, extra, label="LOG START_DUMP")
        try:
            rsp = self.wait_rsp(seq, rsp_timeout)
        except TimeoutError:
            return "[!] Таймаут ответа START_DUMP"

        if len(rsp) < 2:
            return "[!] Пустой ответ START_DUMP"
        status = rsp[0]
        if status != LOG_STATUS_OK:
            name = LOG_STATUS_NAMES.get(status, str(status))
            return f"[!] START_DUMP: {name}"
        if len(rsp) < 16:
            return "[!] Короткий ответ START_DUMP"

        stream_id = struct.unpack_from("<H", rsp, 2)[0]
        total = struct.unpack_from("<I", rsp, 4)[0]
        crit_wh = struct.unpack_from("<I", rsp, 8)[0]
        gen_wh = struct.unpack_from("<I", rsp, 12)[0]
        tier_name = LOG_TIER_NAMES.get(tier, f"T{tier}")
        return stream_id, total, crit_wh, gen_wh, tier_name

    def _iter_tier_stream(
        self,
        stream_id: int,
        tier_name: str,
        stop_event=None,
        data_timeout: float = 15.0,
    ) -> Iterator[str]:
        """Принимать DATA до первой пустой/битой записи или LAST."""
        received = 0
        last_pkt_num = -1
        while True:
            if stop_event is not None and stop_event.is_set():
                yield "[*] Остановлено пользователем"
                return

            try:
                pkt = wait_log_packet(
                    self._queue,
                    data_timeout,
                    lambda p: p["type"] == LOG_PKT_TYPE_DATA,
                )
            except TimeoutError:
                yield "[!] Таймаут ожидания DATA"
                return

            body = pkt["body"]
            try:
                header, records = parse_data_packet(body)
            except ValueError as exc:
                yield f"[!] Ошибка разбора DATA: {exc}"
                return

            if header["stream_id"] != stream_id:
                continue
            if header["pkt_num"] <= last_pkt_num and last_pkt_num >= 0:
                continue
            last_pkt_num = header["pkt_num"]

            flags = header["flags"]
            if flags & LOG_STREAM_FLAG_FIRST:
                yield f"[*] Начало потока ({tier_name})"
            if flags & LOG_STREAM_FLAG_ERROR:
                yield "[!] Ошибка чтения на устройстве"
                return

            for logical_idx, rec_status, rec_tier, rec_bytes in records:
                yield format_event_record(logical_idx, rec_status, rec_tier, rec_bytes)
                if rec_status == EVENT_LOG_REC_VALID:
                    received += 1

            if flags & LOG_STREAM_FLAG_LAST:
                yield f"[*] Секция {tier_name} завершена: {received} событий"
                return

    def iter_dump(
        self,
        tier: int,
        start_logical: int = 0,
        stop_event=None,
        rsp_timeout: float = 8.0,
        data_timeout: float = 15.0,
    ) -> Iterator[str]:
        drain_queue(self._queue)

        tiers: list[tuple[int, str]] = []
        if tier == LOG_TIER_BOTH:
            tiers = [(LOG_TIER_CRITICAL, "КРИТ"), (LOG_TIER_GENERAL, "ОБЩ")]
        elif tier == LOG_TIER_CRITICAL:
            tiers = [(LOG_TIER_CRITICAL, "КРИТ")]
        elif tier == LOG_TIER_GENERAL:
            tiers = [(LOG_TIER_GENERAL, "ОБЩ")]
        else:
            yield f"[!] Неизвестный уровень: {tier}"
            return

        total_received = 0
        for tier_id, tier_name in tiers:
            if stop_event is not None and stop_event.is_set():
                yield "[*] Остановлено пользователем"
                break

            result = self._begin_dump(tier_id, start_logical, rsp_timeout)
            if isinstance(result, str):
                yield result
                break
            stream_id, total, crit_wh, gen_wh, _ = result
            yield (
                f"[*] Поток #{stream_id} ({tier_name}): ожидается до {total} записей, "
                f"head_crit={crit_wh}, head_gen={gen_wh}"
            )
            if total == 0:
                yield f"[*] Журнал ({tier_name}) пуст"
                self.stop_dump()
                continue

            tier_received = 0
            for line in self._iter_tier_stream(
                stream_id, tier_name, stop_event=stop_event, data_timeout=data_timeout
            ):
                if line.startswith("#"):
                    tier_received += 1
                yield line

            total_received += tier_received
            self.stop_dump()

            if stop_event is not None and stop_event.is_set():
                break

            if tier == LOG_TIER_BOTH and tier_id == LOG_TIER_CRITICAL:
                yield "[*] Переход к журналу ОБЩ"

        yield f"[*] Готово: {total_received} событий"
