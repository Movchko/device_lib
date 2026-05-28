#!/usr/bin/env python3
"""
Простой читатель UART/COM для логов ESP32-C3 (ESP-IDF, Arduino Serial и сырой текст).
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from dataclasses import asdict, dataclass
from datetime import datetime
from typing import Iterator, Optional

import serial
from serial.tools import list_ports

# ESP-IDF: I (12345) wifi: Connected
ESP_IDF_RE = re.compile(
    r"^(?P<level>[EWIDV])\s+\((?P<ms>\d+)\)\s+(?P<tag>[\w.:+-]+):\s*(?P<message>.*)$"
)

# Arduino / printf с уровнем: [E][wifi] msg  или  [123][E][tag] msg
BRACKET_RE = re.compile(
    r"^\[(?:(?P<ms>\d+)\])?\]?\[(?P<level>[EWIDV])\]\[(?P<tag>[^\]]+)\]\s*(?P<message>.*)$"
)

LEVEL_NAMES = {
    "E": "ERROR",
    "W": "WARN",
    "I": "INFO",
    "D": "DEBUG",
    "V": "VERBOSE",
}

LEVEL_COLORS = {
    "E": "\033[91m",  # red
    "W": "\033[93m",  # yellow
    "I": "\033[92m",  # green
    "D": "\033[96m",  # cyan
    "V": "\033[90m",  # gray
}
RESET = "\033[0m"


@dataclass
class LogLine:
    raw: str
    level: Optional[str] = None
    level_name: Optional[str] = None
    ms: Optional[int] = None
    tag: Optional[str] = None
    message: Optional[str] = None
    format: str = "raw"  # esp_idf | bracket | raw
    host_time: Optional[str] = None

    def to_dict(self) -> dict:
        return asdict(self)


def parse_line(text: str, add_host_time: bool = False) -> LogLine:
    line = text.rstrip("\r\n")
    entry = LogLine(raw=line)
    if add_host_time:
        entry.host_time = datetime.now().isoformat(timespec="milliseconds")

    m = ESP_IDF_RE.match(line)
    if m:
        entry.format = "esp_idf"
        entry.level = m.group("level")
        entry.level_name = LEVEL_NAMES.get(entry.level, entry.level)
        entry.ms = int(m.group("ms"))
        entry.tag = m.group("tag")
        entry.message = m.group("message")
        return entry

    m = BRACKET_RE.match(line)
    if m:
        entry.format = "bracket"
        entry.level = m.group("level")
        entry.level_name = LEVEL_NAMES.get(entry.level, entry.level)
        if m.group("ms"):
            entry.ms = int(m.group("ms"))
        entry.tag = m.group("tag")
        entry.message = m.group("message")
        return entry

    entry.message = line
    return entry


def level_allowed(entry: LogLine, min_level: Optional[str], tags: set[str]) -> bool:
    order = "VDIWE"
    if min_level and entry.level:
        if order.index(entry.level) < order.index(min_level):
            return False
    if tags and entry.tag and entry.tag not in tags:
        return False
    if tags and not entry.tag:
        return False
    return True


def format_console(entry: LogLine, use_color: bool) -> str:
    if entry.format == "raw":
        return entry.raw

    color = LEVEL_COLORS.get(entry.level or "", "") if use_color else ""
    reset = RESET if use_color else ""
    lvl = entry.level_name or entry.level or "?"
    ms = f"{entry.ms:6d} " if entry.ms is not None else "       "
    tag = (entry.tag or "")[:20].ljust(20)
    msg = entry.message or ""
    return f"{color}{lvl:7} {ms}{tag} {msg}{reset}"


def iter_serial_lines(
    port: serial.Serial, encoding: str = "utf-8", errors: str = "replace"
) -> Iterator[str]:
    buffer = b""
    while True:
        chunk = port.read(port.in_waiting or 1)
        if not chunk:
            time.sleep(0.01)
            continue
        buffer += chunk
        while b"\n" in buffer:
            raw_line, buffer = buffer.split(b"\n", 1)
            yield raw_line.decode(encoding, errors=errors)


def cmd_list_ports() -> None:
    ports = list_ports.comports()
    if not ports:
        print("COM-порты не найдены. Подключите ESP32 по USB.")
        return
    print(f"{'Порт':<8} {'Описание'}")
    print("-" * 60)
    for p in ports:
        desc = p.description or ""
        print(f"{p.device:<8} {desc}")


def run_monitor(args: argparse.Namespace) -> int:
    tag_filter = {t.strip() for t in args.tag.split(",") if t.strip()} if args.tag else set()

    try:
        ser = serial.Serial(
            port=args.port,
            baudrate=args.baud,
            timeout=0.1,
        )
    except serial.SerialException as exc:
        print(f"Не удалось открыть {args.port}: {exc}", file=sys.stderr)
        print("Список портов: python esp_uart_logger.py list", file=sys.stderr)
        return 1

    out_file = open(args.output, "a", encoding="utf-8") if args.output else None
    use_color = args.color and sys.stdout.isatty()

    print(
        f"Слушаю {args.port} @ {args.baud} (Ctrl+C — выход)\n",
        file=sys.stderr,
    )

    try:
        for text in iter_serial_lines(ser, encoding=args.encoding):
            entry = parse_line(text, add_host_time=args.timestamp)
            if not level_allowed(entry, args.min_level, tag_filter):
                continue

            if args.json:
                print(json.dumps(entry.to_dict(), ensure_ascii=False))
            else:
                print(format_console(entry, use_color))

            if out_file:
                out_file.write(entry.raw + "\n")
                out_file.flush()
    except KeyboardInterrupt:
        print("\nОстановлено.", file=sys.stderr)
    finally:
        ser.close()
        if out_file:
            out_file.close()

    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Чтение и разбор UART-логов ESP32-C3 через COM-порт.",
    )
    sub = p.add_subparsers(dest="command", required=True)

    sub.add_parser("list", help="Показать доступные COM-порты")

    mon = sub.add_parser("monitor", help="Читать и разбирать логи")
    mon.add_argument("port", help="COM-порт, например COM5")
    mon.add_argument(
        "-b",
        "--baud",
        type=int,
        default=115200,
        help="Скорость UART (по умолчанию 115200)",
    )
    mon.add_argument(
        "-l",
        "--min-level",
        choices=["V", "D", "I", "W", "E"],
        help="Минимальный уровень: V < D < I < W < E",
    )
    mon.add_argument(
        "-t",
        "--tag",
        help="Фильтр по тегам через запятую, например wifi,main",
    )
    mon.add_argument(
        "-o",
        "--output",
        help="Дополнительно писать сырой лог в файл",
    )
    mon.add_argument("--json", action="store_true", help="Вывод в JSON")
    mon.add_argument(
        "--no-color",
        dest="color",
        action="store_false",
        help="Без цветов в консоли",
    )
    mon.set_defaults(color=True)
    mon.add_argument(
        "--timestamp",
        action="store_true",
        help="Добавить host_time в JSON",
    )
    mon.add_argument(
        "--encoding",
        default="utf-8",
        help="Кодировка UART (по умолчанию utf-8)",
    )

    return p


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    if args.command == "list":
        cmd_list_ports()
        return 0
    if args.command == "monitor":
        return run_monitor(args)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
